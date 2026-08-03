#pragma once

namespace ts {

/// Which output of the state-variable filter a partial takes.
enum class FilterTap {
    /// The partial bypasses the filter entirely.
    bypass,
    /// Lowpass — the `low` integrator.
    low_pass,
    /// Bandpass — the `band` integrator.
    band_pass,
    /// Highpass — `in - q*band - low`.
    high_pass,
    /// Notch — `low - high`.
    notch,
};

/// The engine's filter: a Chamberlin state-variable filter, with all four responses taken as
/// different taps of the same two-integrator loop.
///
/// Read from the disassembly rather than inferred: there is no prescale, no oversampling and no
/// reordering. Per sample,
///
///     low  += f * band;
///     high  = input - (q * band + low);
///     band += f * high;
///
/// Both coefficients come from tables, so no fitted constant appears anywhere in this path. Note
/// that `q` is Chamberlin's reciprocal-Q: *smaller* means more resonant, and the neutral resonance
/// byte gives exactly 1.0.
///
/// The order of those three lines is load-bearing and so is the grouping in the middle one.
/// Reassociating `input - (q*band + low)` into `input - q*band - low` is a different sequence of
/// IEEE operations, and the build disables FMA contraction so the compiler cannot fuse them either.
///
/// **There is an open divergence involving this filter, and it is not in this loop.** Drum tone
/// 1946 (resonance byte 121, `f = 0.654, q = 1.891`) renders +1.1 dB broadband and about 5 dB hot at
/// Nyquist, correlating 0.859 against the reference where its neighbours hold 0.999. Do not try to
/// fix it here. `svf_render_hp` has been read: eight unrolled copies of exactly the recurrence
/// above, same operand ages, same parenthesisation, no branch on `q` anywhere — and `f` and `q`
/// reach it as the values we already match to the integer in steady state, which is where nearly all
/// of the note lives.
///
/// An earlier reading blamed the poles going real (`f*q` past the point where
/// `(2 - f*q - f*f)^2 >= 4*(1 - f*q)`, putting one pole on the negative axis at Nyquist). **That was
/// retracted:** sweeping the part cutoff shows the renders still disagree at cutoffs where no
/// negative pole exists and the filter is nearly transparent, while moving the pole twice as far
/// barely changes the correlation. The pole regime was a correlate of the resonance byte, not the
/// cause. See specv2 `docs/FINDINGS.md`; the evidence now points upstream of the filter.
class StateVariableFilter {
public:
    /// The lowpass integrator's current value.
    [[nodiscard]] double low() const noexcept { return low_; }

    /// The bandpass integrator's current value.
    [[nodiscard]] double band() const noexcept { return band_; }

    /// Clears the filter state.
    void reset() noexcept
    {
        low_ = 0.0;
        band_ = 0.0;
    }

    /// Advances the filter by one sample and returns the selected tap.
    ///
    /// `f` is the frequency coefficient, `2*sin(pi*fc/fs)`; `q` is the reciprocal-Q damping.
    [[nodiscard]] double process(double input, double f, double q, FilterTap tap) noexcept
    {
        low_ += f * band_;
        const double high = input - ((q * band_) + low_);
        band_ += f * high;

        switch (tap) {
        case FilterTap::low_pass:
            return low_;
        case FilterTap::high_pass:
            return high;
        case FilterTap::band_pass:
            return band_;
        case FilterTap::notch:
            return low_ - high;
        case FilterTap::bypass:
            break;
        }
        return input;
    }

private:
    double low_ = 0.0;
    double band_ = 0.0;
};

} // namespace ts
