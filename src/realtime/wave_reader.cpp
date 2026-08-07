#include "realtime/wave_reader.hpp"

#include "dsp/fixed.hpp"
#include "dsp/wave_codec.hpp"

#include <cmath>

namespace ts {

void WaveReader::start(const DecodedWave& wave)
{
    wave_ = &wave;
    position_ = 0.0;
    finished_ = false;
    loop_event_ = false;
    buffer_ = wave.is_looping() ? std::span<const float>{wave.loop_buffer}
                                : std::span<const float>{wave.samples};

    if (wave.mode == SamplerMode::ping_pong) {
        // Deliberately NOT cleared. The reference allocates this ring once per voice object and
        // reuses it, so a recycled voice starts with the previous note's samples still in it -- and
        // the 4-tap window reads one sample behind the read index, which on the very first sample
        // is index -1, i.e. the far end of the ring. Clearing it here is arguably tidier and is a
        // different render.
        generated_ = 0;
        path_index_ = 0;
        path_leg_ = 0;
        path_ended_ = false;
        // From the preamble seed, not zero: the decoder's predictor enters the data start
        // carrying the block-boundary preamble's integral, and an unaligned wave rides it as a
        // constant -- the crash's -0.041015625, measured against the module's own trace.
        predictor_ = wave.seed;
    }
}

void WaveReader::stop() noexcept
{
    wave_ = nullptr;
    buffer_ = {};
    finished_ = true;
}

float WaveReader::next(double ratio) noexcept
{
    const DecodedWave* wave = wave_;
    if (wave == nullptr || finished_) {
        return 0.0F;
    }

    const float sample =
        wave->mode == SamplerMode::ping_pong ? read_ping_pong(ratio) : read_linear(*wave, ratio);
    position_ += ratio;

    // Raised on the loop point the module's samplers actually compare against. `param_1[2]` is
    // sampler state +0x20, so the compared field is +0x2c -- which `scdec sampstate` dumps, and
    // which reads 3026 for prog 73 note 60 against this port's `loop_start` of 3026 exactly. It is
    // the loop *start*; +0x30 is the far end. Tested before the wrap below, which pulls the
    // position back and would hide a crossing a large increment jumped in one step.
    if (!loop_event_ && position_ >= static_cast<double>(wave->loop_start)) {
        loop_event_ = true;
    }

    if (wave->is_looping()) {
        // Subtracting the period is the same wrap the offline path takes modulo, and it keeps the
        // fractional phase exact however long the note holds.
        const double limit = wave->data_end + 1;
        const int period = wave->loop_period();
        while (position_ >= limit) {
            position_ -= period;
        }
    }

    return sample;
}

float WaveReader::read_linear(const DecodedWave& wave, double ratio) noexcept
{
    if (wave.is_looping()) {
        return extended_ ? SincInterpolator::shared().sample(buffer_, position_, ratio)
                         : interpolator_->sample(buffer_, position_);
    }

    // One-shot: hold the last sample, then go silent. The engine's own gate is the read position
    // reaching the end of the data, not the envelope.
    const double end = wave.data_end - 1;
    if (position_ >= end) {
        finished_ = true;
        return 0.0F;
    }

    return extended_ ? SincInterpolator::shared().sample(buffer_, position_, ratio)
                     : interpolator_->sample(buffer_, position_);
}

float WaveReader::read_ping_pong(double ratio) noexcept
{
    const auto index = static_cast<std::int64_t>(std::floor(position_));
    // The wide kernel reaches further ahead than the 4-tap one, so more of the stream has to exist
    // before it is read. The *end* test below still uses two, so a note's length does not change
    // with the interpolator -- only how much of the tail is available to filter with.
    generate(index + (extended_ ? SincInterpolator::max_radius : 2));

    if (path_ended_ && index + 2 >= generated_) {
        finished_ = true;
        return 0.0F;
    }

    const double fraction = position_ - static_cast<double>(index);
    return extended_ ? SincInterpolator::shared().sample_ring(ring_, index, fraction, ratio)
                     : interpolator_->sample_ring(ring_, index, fraction);
}

void WaveReader::generate(std::int64_t up_to)
{
    const DecodedWave& wave = *wave_;
    constexpr std::int64_t mask = ping_pong_window - 1;

    while (generated_ <= up_to && !path_ended_) {
        const int index = path_index_;

        switch (path_leg_) {
        case 0:
            ++path_index_;
            if (path_index_ > wave.data_end) {
                if (wave.data_end <= wave.loop_start) {
                    path_ended_ = true;
                } else {
                    path_leg_ = 1;
                    path_index_ = wave.data_end;
                }
            }
            break;

        case 1:
            --path_index_;
            if (path_index_ < wave.loop_start) {
                path_leg_ = 2;
                path_index_ = wave.loop_start;
            }
            break;

        default:
            ++path_index_;
            if (path_index_ > wave.data_end) {
                path_leg_ = 1;
                path_index_ = wave.data_end;
            }
            break;
        }

        predictor_ = fx::wadd(predictor_, wave.steps[static_cast<std::size_t>(index)]);
        ring_[static_cast<std::size_t>(generated_ & mask)] =
            static_cast<float>(static_cast<double>(predictor_) * codec::output_scale);
        ++generated_;
    }
}

} // namespace ts
