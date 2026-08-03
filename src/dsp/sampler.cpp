#include "tabulasonora/sampler.hpp"

#include "dsp/fixed.hpp"
#include "dsp/wave_codec.hpp"

#include <algorithm>
#include <cmath>

namespace ts {
namespace {

/// Packs the three descriptor fields that determine a wave's bytes into one cache key.
///
/// Region is 7 bits and the two positions are 20 bits each, so 47 bits is the whole key and no hash
/// combiner is needed.
[[nodiscard]] constexpr std::uint64_t cache_key(const WaveDescriptor& descriptor) noexcept
{
    return (static_cast<std::uint64_t>(descriptor.region & 0x7F) << 40)
           | (static_cast<std::uint64_t>(descriptor.loop & 0xFFFFF) << 20)
           | static_cast<std::uint64_t>(descriptor.start & 0xFFFFF);
}

/// Floating-point modulo that always returns a non-negative result.
[[nodiscard]] double positive_modulo(double value, int period) noexcept
{
    const double result = std::fmod(value, static_cast<double>(period));
    return result < 0.0 ? result + period : result;
}

} // namespace

const DecodedWave* Sampler::decode(const WaveDescriptor& descriptor)
{
    const std::uint64_t key = cache_key(descriptor);

    const auto found = cache_.find(key);
    if (found != cache_.end()) {
        return found->second.get();
    }

    // A descriptor with no usable data caches a null so the ROM is not re-read for it every note.
    auto wave = decode_core(descriptor);
    const DecodedWave* result = wave.get();
    cache_.emplace(key, std::move(wave));
    return result;
}

std::unique_ptr<DecodedWave> Sampler::decode_core(const WaveDescriptor& descriptor) const
{
    const auto streams = rom_->read_streams(descriptor.region, descriptor.loop, descriptor.start);
    if (!streams) {
        return nullptr;
    }

    const auto sample_count = static_cast<std::size_t>(streams->sample_count);

    auto wave = std::make_unique<DecodedWave>();

    // One extra step: the ping-pong sampler applies the delta at the turnaround index.
    wave->steps.resize(sample_count + 1);
    for (std::size_t i = 0; i < wave->steps.size(); ++i) {
        wave->steps[i] = codec::step(
            streams->delta[i],
            codec::scale_at(streams->scale, streams->scale_phase + static_cast<int>(i)));
    }

    // Decode one sample past the data end. The forward loop is inclusive of that index -- its
    // period is data_end - loop_start + 1 -- so the sample genuinely exists in the delta stream and
    // is needed to close the loop. Stopping one short makes the wrap substitute the loop's first
    // sample instead, which then plays twice per pass; on a short single-cycle loop that is an
    // audible click every period. The Python reference stops one short here and the hardware does
    // not, which is one of the documented places this engine follows the hardware.
    wave->samples.resize(sample_count + 1);
    std::int32_t predictor = 0;
    for (std::size_t i = 0; i <= sample_count; ++i) {
        predictor = fx::wadd(predictor, wave->steps[i]);
        wave->samples[i] = static_cast<float>(static_cast<double>(predictor) * codec::output_scale);
    }

    wave->loop_start = std::max(0, descriptor.end - streams->data_start);
    wave->data_end = streams->sample_count;

    // A reverse wave is the same data read the other way, so it is turned round here rather than
    // given a downward-walking read path. Two things fall out of that. Both renderers get it at
    // once, because they share this sampler and neither knows the difference; and the seam problems
    // that make the engine's own backwards walk delicate do not arise, because the predictor is
    // integrated forward exactly as for any other wave and only the finished samples are reversed.
    //
    // Playing one is always a one-shot. Statically 202 of the 218 reverse descriptors already
    // collapse end onto start, and the engine reconfigures the rest to match.
    if (descriptor.reverse()) {
        std::reverse(wave->samples.begin(), wave->samples.end());
        wave->mode = SamplerMode::one_shot;
        wave->reversed = true;
        return wave;
    }

    // The descriptor's loop flag is NOT what gates looping: it reads zero for piano, so trusting it
    // makes held notes run out as one-shots. What decides is whether a sustain region exists.
    wave->mode = descriptor.ping_pong()                         ? SamplerMode::ping_pong
                 : streams->sample_count - wave->loop_start > 0 ? SamplerMode::loop
                                                                : SamplerMode::one_shot;

    if (wave->mode == SamplerMode::loop && streams->sample_count > wave->loop_start + 1) {
        wave->loop_buffer = build_loop_buffer(*wave);
    }
    return wave;
}

std::vector<float> Sampler::play(const DecodedWave& wave, int sample_count, double ratio) const
{
    std::vector<double> positions(static_cast<std::size_t>(std::max(0, sample_count)));
    for (std::size_t i = 0; i < positions.size(); ++i) {
        positions[i] = static_cast<double>(i) * ratio;
    }
    return play_at(wave, positions);
}

std::vector<float> Sampler::play_variable(const DecodedWave& wave,
                                          int sample_count,
                                          std::span<const double> ratios) const
{
    std::vector<double> positions(static_cast<std::size_t>(std::max(0, sample_count)));
    double accumulated = 0.0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        positions[i] = accumulated;
        const double rate = ratios.empty() ? 1.0 : ratios[std::min(i, ratios.size() - 1)];
        accumulated += rate;
    }
    return play_at(wave, positions);
}

std::vector<float> Sampler::play_at(const DecodedWave& wave,
                                    std::span<const double> positions) const
{
    std::vector<float> output(positions.size());
    if (positions.empty()) {
        return output;
    }

    const int loop_start = wave.loop_start;
    const int data_end = wave.data_end;

    if (wave.mode == SamplerMode::ping_pong) {
        const std::vector<float> buffer =
            build_ping_pong_buffer(wave, static_cast<int>(positions.back()) + 3);
        interpolator_->resample(buffer, positions, output);
        return output;
    }

    if (wave.is_looping()) {
        const int period = wave.loop_period();
        std::vector<double> wrapped(positions.size());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const double p = positions[i];
            wrapped[i] =
                p < data_end + 1 ? p : loop_start + positive_modulo(p - (data_end + 1), period);
        }

        interpolator_->resample(wave.loop_buffer, wrapped, output);
        return output;
    }

    // One-shot: hold the last sample, then go silent.
    std::vector<double> held(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        held[i] = std::min(positions[i], static_cast<double>(data_end - 1));
    }

    interpolator_->resample(wave.samples, held, output);

    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (held[i] >= data_end - 1) {
            output[i] = 0.0F;
        }
    }

    return output;
}

std::vector<float> Sampler::build_loop_buffer(const DecodedWave& wave)
{
    // Both the data and the repeated region are inclusive of the data end, matching the loop's own
    // period.
    const int period = wave.loop_period();
    const auto required = static_cast<std::size_t>(wave.data_end + 4);

    std::vector<float> buffer;
    buffer.reserve(required);

    for (int i = 0; i <= wave.data_end && static_cast<std::size_t>(i) < wave.samples.size(); ++i) {
        buffer.push_back(wave.samples[static_cast<std::size_t>(i)]);
    }

    while (buffer.size() < required && period > 0) {
        for (int i = wave.loop_start; i <= wave.data_end && buffer.size() < required; ++i) {
            buffer.push_back(wave.samples[static_cast<std::size_t>(i)]);
        }
    }

    return buffer;
}

std::vector<float> Sampler::build_ping_pong_buffer(const DecodedWave& wave, int required_samples)
{
    const auto required = static_cast<std::size_t>(std::max(1, required_samples));

    std::vector<int> indices;
    indices.reserve(required + 8);

    for (int i = 0; i <= wave.data_end && indices.size() < required; ++i) {
        indices.push_back(i);
    }

    while (indices.size() < required) {
        for (int i = wave.data_end; i >= wave.loop_start && indices.size() < required; --i) {
            indices.push_back(i);
        }
        for (int i = wave.loop_start; i <= wave.data_end && indices.size() < required; ++i) {
            indices.push_back(i);
        }
        if (wave.data_end <= wave.loop_start) {
            break;
        }
    }

    // The index is unchanged on a turnaround, so that sample's delta is applied twice; and the
    // predictor keeps accumulating in both directions rather than subtracting, which makes the
    // backward leg the wave inverted and time-reversed. Both turnarounds are continuous by
    // construction -- there is no seam and no phase jump.
    std::vector<float> buffer(indices.size());
    std::int32_t predictor = 0;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        predictor = fx::wadd(predictor, wave.steps[static_cast<std::size_t>(indices[i])]);
        buffer[i] = static_cast<float>(static_cast<double>(predictor) * codec::output_scale);
    }

    return buffer;
}

} // namespace ts
