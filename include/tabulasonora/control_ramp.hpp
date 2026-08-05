#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace ts {

/// The engine's anti-zipper control smoother — `voice_ctrl_ramp_b` @`18005e990`.
///
/// A part's volume is not handed to the mixer the moment CC#7 moves. Every voice carries a second
/// per-sample gain buffer beside its TVA envelope, and `render_block` fills it through this ramp
/// from the target `voice_volume_apply` computes. Without it a volume slide is a staircase of
/// instantaneous gain jumps, one per event, each a waveform discontinuity — audible as a click on
/// any sustained part. DOOM's `D_DOOM.MUS` is the case that exposes it: two pad channels crossfaded
/// by nothing but CC#7, restepped every seven ticks (~50 ms) for four minutes.
///
/// **The approach is exponential, not linear.** Each update closes a fixed *fraction* of the
/// remaining distance:
///
/// ```
/// error = (int16)((target << 10) - accumulator) >> 13)
/// step  = error * rate, forced to at least one accumulator LSB either way
/// ```
///
/// so with the volume path's `rate` of `0xCC` an update closes 204/8192 — 2.49% — of what is left,
/// and updates land every eight samples. That is a time constant of 317 samples, 9.9 ms at 32 kHz,
/// and a settle around 45 ms. The `minimum_step` floor is what guarantees arrival: a proportional
/// step alone would stall once the error shifted down to zero.
///
/// Measured against the oracle rather than assumed. `scdec volramp` reads the engine's own gain
/// buffer through a CC#7 127->0 step and it falls by a *constant ratio* of 0.90403 per 32-sample
/// call — which is `(1 - 204/8192)^4`, four updates a call. A constant ratio is the signature of a
/// step rescaled from the live error; a fixed decrement would fall linearly. `voice_ctrl_ramp_a`,
/// the envelope's smoother, is the fixed-decrement one.
///
/// This is the sibling of `voice_ctrl_ramp_a`, which smooths the *envelope* gain and is a plain
/// linear ramp. The two are not interchangeable and the engine runs both, one per gain buffer.
///
/// **The rest state dithers, and that is the engine's own behaviour.** The ramp is stepped
/// unconditionally — there is no early-out once the target is reached — so at rest the error is
/// zero, `minimum_step` pushes the accumulator up one LSB, the next update's error is -1 and pushes
/// it back. The accumulator alternates over a 1024-wide span that the gain's own `>> 13` swallows
/// for seven target words in eight. The oracle's own rest value creeps up about 1/16384 per 64
/// samples before a retarget truncates it back, which this does not reproduce exactly; it is 0.07%
/// of level and the trace cannot say what resets it.
class ControlRamp {
public:
    /// `voice_volume_apply`'s rate word for the part-volume path, hard-coded at the call site.
    static constexpr int volume_rate_word = 0xCC;

    /// The accumulator carries ten bits below the value it tracks.
    static constexpr int accumulator_shift = 10;

    /// The error is taken thirteen bits down before it meets the rate.
    static constexpr int error_shift = 13;

    /// The smallest move an update may make, in accumulator units.
    static constexpr int minimum_step = 0x400;

    /// Output samples one update covers, before the ZOH mask divides it further.
    ///
    /// `render_block` calls the ramp once per eight-sample sub-chunk and the value is held flat
    /// across them, so the smoother's clock is the sub-chunk and not the sample. **This is measured,
    /// not read off the tables**: the rate word's divider bits select mask zero, which taken alone
    /// would mean one update a sample and a glide eight times too fast. Against the oracle, a single
    /// CC#7 127->0 step decays with a time constant of 343 samples; one update a sample predicts 40.
    /// The eight-sample sub-chunk is also visible directly in the gain buffer, which holds eight
    /// copies of one value per call.
    static constexpr int samples_per_update = 8;

    /// The gain is read out of the accumulator thirteen bits down, in 1/16384ths.
    static constexpr int gain_shift = 13;
    static constexpr double gain_scale = 0x1p-14;

    /// What the engine emits for a zero value: not quite silence, and deliberately so.
    static constexpr float floor_gain = 1e-05F;

    /// The ramp's target for a volume word — `CONCAT44(0xcc, voice_volume_apply() << 2)`.
    ///
    /// The two low bits the shift adds are not decoration: the gain is read back out of the
    /// accumulator `>> 13` while the accumulator carries the value `<< 10`, so a target that
    /// skipped the shift would render every part exactly two stops down.
    [[nodiscard]] static constexpr int target_of(int volume_word) noexcept
    {
        return volume_word << 2;
    }

    /// The zero-order-hold mask a rate word selects, through the two tables that pick it.
    ///
    /// `g_ramp_flagword` turns the rate word's bits 12-13 into the flag bits 3-4 that then index
    /// `g_ramp_divider`. The volume path's `0xCC` lands on index zero, so the mask is zero and adds
    /// no hold of its own — the eight-sample cadence in `samples_per_update` is the whole of it.
    [[nodiscard]] static unsigned mask_of(int rate_word,
                                          std::span<const std::uint8_t> flagword,
                                          std::span<const std::uint8_t> divider) noexcept
    {
        const auto selector = static_cast<std::size_t>((rate_word >> 12) & 3);
        const unsigned flags = selector * 4 < flagword.size() ? flagword[selector * 4] : 0U;
        const auto index = static_cast<std::size_t>((flags >> 3) & 3);
        return index < divider.size() ? divider[index] : 0U;
    }

    /// Points the ramp at a new target — `voice_ramp_target_aux` @`18008a5e0`.
    ///
    /// Called once a control tick, not once a block. It reseeds the accumulator from the tracked
    /// value, which quantises away whatever sub-LSB residue the last tick's stepping left; that
    /// truncation is part of the behaviour and is why this is driven off the tick and not the block.
    void retarget(int target, int rate_word, unsigned mask) noexcept
    {
        rate_ = rate_word & 0xFFF;
        mask_ = mask;
        counter_ = 0;
        phase_ = 0;
        target_ = target;
        accumulator_ = current_ << accumulator_shift;
        held_ = decode(current_, accumulator_);
        active_ = current_ != target_;
    }

    /// Seeds the ramp at a value, with no glide — `tvf_env_prep` writes the same value to both the
    /// source and the target slot, so a voice starting mid-slide begins at the level already
    /// reached rather than sweeping up to it.
    void seed(int value, int rate_word, unsigned mask) noexcept
    {
        current_ = value;
        retarget(value, rate_word, mask);
    }

    /// Advances one sample and returns the gain it lands on.
    [[nodiscard]] float step() noexcept
    {
        // Arrived: the ramp deactivates and the last gain stands until something retargets it.
        //
        // Not cosmetic. `minimum_step` moves the accumulator whether or not there is any error
        // left, so a ramp that kept stepping at rest would walk straight past its target and out
        // the far side -- to a *negative* gain, one sample in two, phase-inverted at -84 dB. The
        // oracle's fade lands on 1e-05 and holds it flat for as long as the trace runs.
        if (!active_) {
            return held_;
        }

        // Held flat within a sub-chunk; the ramp's own clock only ticks at the boundary.
        if (++phase_ < samples_per_update) {
            return held_;
        }
        phase_ = 0;

        ++counter_;
        if ((static_cast<unsigned>(counter_) & mask_) != 0) {
            return held_;
        }

        const auto error = static_cast<std::int16_t>(
            ((target_ << accumulator_shift) - accumulator_) >> error_shift);
        int step = static_cast<int>(error) * rate_;
        if (step < 0) {
            step = std::min(step, -minimum_step);
        } else {
            step = std::max(step, minimum_step);
        }

        accumulator_ += step;
        current_ = accumulator_ >> accumulator_shift;
        active_ = current_ != target_;
        held_ = decode(current_, accumulator_);
        return held_;
    }

    /// Fills a block's worth of per-sample gains.
    void fill(std::span<double> gains) noexcept
    {
        for (double& gain : gains) {
            gain = static_cast<double>(step());
        }
    }

    /// The value the ramp currently tracks, in the target's own units.
    [[nodiscard]] int current() const noexcept { return current_; }

private:
    /// The engine reads the gain out of the *accumulator*, not the tracked value, and narrows to
    /// `int16` on the way — so the ten bits the accumulator carries below the value are not simply
    /// discarded, three of them survive into the gain.
    [[nodiscard]] static float decode(int current, int accumulator) noexcept
    {
        if (current == 0) {
            return floor_gain;
        }
        const auto narrowed = static_cast<std::int16_t>(accumulator >> gain_shift);
        return static_cast<float>(static_cast<double>(narrowed) * gain_scale);
    }

    int rate_ = 0;
    unsigned mask_ = 0;
    int phase_ = 0;
    int counter_ = 0;
    int current_ = 0;
    int target_ = 0;
    int accumulator_ = 0;
    float held_ = 0.0F;
    bool active_ = false;
};

} // namespace ts
