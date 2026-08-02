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
        predictor_ = 0;
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
        wave->mode == SamplerMode::ping_pong ? read_ping_pong() : read_linear(*wave);
    position_ += ratio;

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

float WaveReader::read_linear(const DecodedWave& wave) noexcept
{
    if (wave.is_looping()) {
        return interpolator_->sample(buffer_, position_);
    }

    // One-shot: hold the last sample, then go silent. The engine's own gate is the read position
    // reaching the end of the data, not the envelope.
    const double end = wave.data_end - 1;
    if (position_ >= end) {
        finished_ = true;
        return 0.0F;
    }

    return interpolator_->sample(buffer_, position_);
}

float WaveReader::read_ping_pong() noexcept
{
    const auto index = static_cast<std::int64_t>(std::floor(position_));
    generate(index + 2);

    if (path_ended_ && index + 2 >= generated_) {
        finished_ = true;
        return 0.0F;
    }

    return interpolator_->sample_ring(ring_, index, position_ - static_cast<double>(index));
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
