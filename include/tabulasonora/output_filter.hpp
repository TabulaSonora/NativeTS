#pragma once

#include <cstddef>
#include <span>

namespace ts {

/// The module's output stage — `tg_output_filter` @`18008aca0`.
///
/// `TG_Process` runs this over every 32-sample chunk on its way out, and it runs **whether or not
/// the host rate matches the engine's**: at 32 kHz the ratio it carries is exactly 1 and it still
/// filters. That is worth knowing because it sits in every latency measured between this engine and
/// the module, and was the first suspect for the 20 samples left over once the event pipeline
/// accounted for 128 of them.
///
/// The shape is a half-sample allpass feeding a linear interpolator:
///
/// ```
/// mid = allpass(x)                       // one pole, one zero, k = 1/3
/// out = frac < 0.5 ? lerp(prev, mid, 2 * frac)
///                  : lerp(mid, next, 2 * frac - 1)
/// ```
///
/// So the allpass supplies the half-way point between two input samples and the interpolator picks
/// a side of it. The coefficient is `1/3` at every host rate tried -- 22.05, 32, 44.1 and 48 kHz
/// all read it back unchanged -- so it is a constant of the filter rather than a function of the
/// conversion; only the ratio moves, and it is `32000 / host`.
///
/// **At 1:1 this reduces to a one-sample delay.** The phase accumulator sits at zero, so the
/// interpolation weight is zero, the output is the previous input, and the allpass contributes
/// nothing to what is heard. That is not a reason to leave it out -- it is a real sample of latency
/// against the oracle, and the filter stops being a plain delay the moment the ratio is not one.
///
/// Read as a resampler rather than a filter, the shape explains itself: the allpass supplies the
/// odd-index sample of a 2x oversampled grid and the lerp runs on that grid, so this is a **band-
/// limited linear interpolator** -- most of a real interpolating filter's stopband for one
/// multiply-add. `1/3` is `(1-d)/(1+d)` at `d = 0.5`, which is why it does not move with the rate.
///
/// **This is not a candidate for a peak or gain discrepancy against the oracle, and was measured
/// out as one on 2026-08-08.** Every probe and both fixture generators run the DLL at 32 kHz, where
/// the ratio is 1 and the table above applies: peak and rms both unchanged to six figures. It only
/// starts colouring anything off 1:1 -- +1.5% peak at 44.1 kHz, +1.0% at 48 kHz. The module seeds
/// its phase at `1e-5` where this seeds at `0.0`, and at 32 kHz that difference is worth a max
/// sample delta of 5.4e-06, 0.000365% of peak. It cannot account for the two open
/// peak-moves-but-rms-does-not cases, which need ~49% and ~30%. specv2's FINDINGS carries the
/// working under *The output stage is a 2x oversampled linear interpolator*.
class OutputFilter {
public:
    /// The allpass coefficient, constant across host rates.
    static constexpr double allpass_coefficient = 1.0 / 3.0;

    /// The engine's own rate; the ratio is this over the host's.
    static constexpr int engine_rate = 32000;

    void reset() noexcept
    {
        state_left_ = 0.0;
        state_right_ = 0.0;
        previous_left_ = 0.0;
        previous_right_ = 0.0;
        current_left_ = 0.0;
        current_right_ = 0.0;
        mid_left_ = 0.0;
        mid_right_ = 0.0;
        phase_ = 0.0;
    }

    /// Sets the conversion ratio from the host's sample rate.
    void set_host_rate(int host_rate) noexcept
    {
        ratio_ = host_rate > 0 ? static_cast<double>(engine_rate) / host_rate : 1.0;
    }

    [[nodiscard]] double ratio() const noexcept { return ratio_; }

    /// Takes one input frame: runs the allpass over it and keeps it as the pair the interpolation
    /// sits between.
    ///
    /// Separate from producing output so that a ratio other than one can be served — see
    /// `advance`. `process` below is these three steps in the order that gives one out per in.
    void push(float left, float right) noexcept
    {
        previous_left_ = current_left_;
        previous_right_ = current_right_;

        current_left_ = static_cast<double>(left);
        current_right_ = static_cast<double>(right);

        // The allpass, per channel: the state takes the input less the fed-back state, and the
        // output leads with the coefficient. Half a sample of delay, which is the midpoint the
        // interpolation is taken against.
        const double held_left = state_left_;
        const double held_right = state_right_;
        state_left_ = current_left_ - (allpass_coefficient * held_left);
        state_right_ = current_right_ - (allpass_coefficient * held_right);
        mid_left_ = (state_left_ * allpass_coefficient) + held_left;
        mid_right_ = (state_right_ * allpass_coefficient) + held_right;
    }

    /// The output frame at the phase the filter currently stands at.
    ///
    /// The interpolation runs on the 2x grid the allpass supplies: below the half-way point it
    /// runs from the previous input to the midpoint, above it from the midpoint to this one.
    [[nodiscard]] std::pair<float, float> at() const noexcept
    {
        double out_left = 0.0;
        double out_right = 0.0;
        if (phase_ >= 0.5) {
            const double t = (phase_ + phase_) - 1.0;
            out_left = ((current_left_ - mid_left_) * t) + mid_left_;
            out_right = ((current_right_ - mid_right_) * t) + mid_right_;
        } else {
            const double t = phase_ + phase_;
            out_left = ((mid_left_ - previous_left_) * t) + previous_left_;
            out_right = ((mid_right_ - previous_right_) * t) + previous_right_;
        }
        return {static_cast<float>(out_left), static_cast<float>(out_right)};
    }

    /// Steps the phase on by one output frame, and says how many input frames that needs.
    ///
    /// At the engine's own rate the ratio is one and the answer is always one, which is the
    /// one-in-one-out case `process` is written for. Below one -- every host rate above 32 kHz --
    /// it is one or nothing, and the phase carries the fraction between output frames.
    [[nodiscard]] int advance() noexcept
    {
        phase_ += ratio_;
        int wanted = 0;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            ++wanted;
        }
        return wanted;
    }

    /// Passes one stereo sample through, returning the pair the host receives.
    ///
    /// One in, one out, which is the engine's own rate: it renders at 32 kHz and the ratio is one,
    /// so `advance` always asks for exactly the frame the next call brings. For a host running at
    /// any other rate the counts differ and the caller drives `push`, `at` and `advance` itself.
    [[nodiscard]] std::pair<float, float> process(float left, float right) noexcept
    {
        push(left, right);
        const auto out = at();
        static_cast<void>(advance());
        return out;
    }

private:
    double ratio_ = 1.0;

    /// The allpass state, one per channel.
    double state_left_ = 0.0;
    double state_right_ = 0.0;

    /// The two input frames the interpolation runs between, and the half-way point the allpass
    /// puts between them.
    double previous_left_ = 0.0;
    double previous_right_ = 0.0;
    double current_left_ = 0.0;
    double current_right_ = 0.0;
    double mid_left_ = 0.0;
    double mid_right_ = 0.0;

    double phase_ = 0.0;
};

} // namespace ts
