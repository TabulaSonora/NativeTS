#pragma once

#include "tabulasonora/interpolator.hpp"
#include "tabulasonora/sampler.hpp"

#include <array>
#include <cstdint>

namespace ts {

/// Reads one wave sample at a time, holding its own position between blocks.
///
/// The offline path resamples a whole note at once; this reads it one sample at a time because a
/// live note has no known length. The two share the decoded wave and the same interpolator, so what
/// differs is only when the position is known.
class WaveReader {
public:
    /// Samples of ping-pong stream kept live. A power of two, so the ring masks.
    static constexpr int ping_pong_window = 1024;

    explicit WaveReader(const Interpolator& interpolator) : interpolator_(&interpolator) {}

    /// Whether the wave has run out and is now silent.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /// Whether the read position has reached the wave's loop point.
    ///
    /// The module's decoder raises an event on that transition -- `voice_report_finished` turns it
    /// into `voice+4`, and the control tick after that adopts the wave's second fine tune. See
    /// `WaveDescriptor::second_fine_tune`. Latched, because the event fires once: a looping wave
    /// crosses the point again every period and the module does not raise it again.
    [[nodiscard]] bool passed_loop_start() const noexcept { return passed_loop_start_; }

    /// Starts a wave from its beginning.
    void start(const DecodedWave& wave);

    /// Releases the wave so nothing is held between notes.
    void stop() noexcept;

    /// Reads one sample and advances by a playback rate.
    [[nodiscard]] float next(double ratio) noexcept;

private:
    [[nodiscard]] float read_linear(const DecodedWave& wave) noexcept;
    [[nodiscard]] float read_ping_pong() noexcept;

    /// Extends the ping-pong stream far enough to cover a read at an index.
    ///
    /// The index walks up to the data end, turns around and walks back to the loop point, then
    /// turns around again. The index is unchanged on a turnaround, so that sample's delta is
    /// applied twice; and the predictor accumulates in both directions rather than subtracting,
    /// which makes the backward leg the wave inverted and time-reversed. Both turnarounds are
    /// continuous by construction — there is no seam and no phase jump.
    void generate(std::int64_t up_to);

    const Interpolator* interpolator_;
    const DecodedWave* wave_ = nullptr;
    std::span<const float> buffer_;

    double position_ = 0.0;
    bool finished_ = true;
    bool passed_loop_start_ = false;

    std::array<float, ping_pong_window> ring_{};
    std::int64_t generated_ = 0;
    int path_index_ = 0;
    int path_leg_ = 0;
    bool path_ended_ = false;
    std::int32_t predictor_ = 0;
};

} // namespace ts
