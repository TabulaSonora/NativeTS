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

    /// Reads through the wide band-limiting kernel instead of the module's 4-tap one.
    ///
    /// Off is the module. On is `ToneGeneratorOptions::extended_interpolation`, and it is the only
    /// thing that makes lifting the pitch increment ceiling safe -- see `SincInterpolator`.
    void set_extended(bool extended) noexcept { extended_ = extended; }

    /// Whether the wave has run out and is now silent.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /// Takes the wave's pending loop event, if one has been raised.
    ///
    /// The module's decoder raises a flag when the read position reaches the loop point;
    /// `voice_report_finished` @18008aec0 scans it, clears it, and sets `voice+4`; and
    /// `voices_control_update` drains `voice+4` on the next control tick, which is where the
    /// wave's second fine tune is adopted. See `WaveDescriptor::second_fine_tune`.
    ///
    /// Consumed on read, so the reaction is driven by an event drained once rather than by a
    /// condition polled every tick -- the same shape the module has, and what keeps the adoption
    /// tied to the crossing instead of to whatever else happens to be true later.
    ///
    /// The loop *start*: `param_1[2]+0xc` is sampler state +0x2c, which reads 3026 for prog 73
    /// note 60 against this port's `loop_start` of 3026. Retriggering it on the far end instead
    /// was measured and cost an assertion on the gate.
    [[nodiscard]] bool take_loop_event() noexcept
    {
        const bool raised = loop_event_;
        loop_event_ = false;
        return raised;
    }

    /// Starts a wave from its beginning.
    void start(const DecodedWave& wave);

    /// Releases the wave so nothing is held between notes.
    void stop() noexcept;

    /// Reads one sample and advances by a playback rate.
    [[nodiscard]] float next(double ratio) noexcept;

private:
    [[nodiscard]] float read_linear(const DecodedWave& wave, double ratio) noexcept;
    [[nodiscard]] float read_ping_pong(double ratio) noexcept;

    /// Extends the ping-pong stream far enough to cover a read at an index.
    ///
    /// The index walks up to the data end, turns around and walks back to the loop point, then
    /// turns around again. The index is unchanged on a turnaround, so that sample's delta is
    /// applied twice; and the predictor accumulates in both directions rather than subtracting,
    /// which makes the backward leg the wave inverted and time-reversed. Both turnarounds are
    /// continuous by construction — there is no seam and no phase jump.
    void generate(std::int64_t up_to);

    const Interpolator* interpolator_;
    bool extended_ = false;
    const DecodedWave* wave_ = nullptr;
    std::span<const float> buffer_;

    double position_ = 0.0;
    bool finished_ = true;
    bool loop_event_ = false;

    std::array<float, ping_pong_window> ring_{};
    std::int64_t generated_ = 0;
    int path_index_ = 0;
    int path_leg_ = 0;
    bool path_ended_ = false;
    std::int32_t predictor_ = 0;
};

} // namespace ts
