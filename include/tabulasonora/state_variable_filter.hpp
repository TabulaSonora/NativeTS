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
/// **Known divergence in the overdamped regime.** When `f*q` grows past the point where
/// `(2 - f*q - f*f)^2 >= 4*(1 - f*q)`, the state matrix's poles go real and one of them lands on the
/// negative axis — at Nyquist, where it lifts the top octave. The reference engine does not do that.
/// Measured on drum tone 1946 (`f = 0.654, q = 1.891`, poles `+0.682` and `-0.348`): we run about
/// 5 dB hot at Nyquist and +1.1 dB broadband, and the correlation against the DLL falls to 0.859
/// where its underdamped neighbours hold 0.999. Only 19 of the 3352 filtered partials in the library
/// reach the regime at all, so most of the corpus never sees it. What the reference does instead is
/// not yet known — refitting `q` alone recovers most but not all of the gap, so do not "fix" this by
/// clamping until stage D's overdamped branch has actually been read. See specv2 `docs/FINDINGS.md`.
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
