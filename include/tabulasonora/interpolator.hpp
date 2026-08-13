#pragma once

#include "tabulasonora/table_set.hpp"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ts {

/// The sampler's 4-tap FIR resampler — the single most timbre-defining element of the engine.
///
/// 128 phase rows of four coefficients, indexed by the top seven bits of the fractional phase, with
/// no interpolation between rows. Every row sums to 1.0 — to within 1e-5, measured: the sums span
/// 0.999999999970896 to 1.000009999999747, so a flat input comes back flat to about five decimal
/// places and not to float epsilon.
///
/// The row at zero fractional phase is `[0.174, 0.653, 0.173, 1e-5]` — not a passthrough. That mild
/// lowpass is applied at *every* sample regardless of pitch, and it is why linear interpolation
/// sounds measurably brighter: at a quarter of the sample rate this kernel passes 0.638 where
/// linear passes 0.707.
///
/// Deliberately scalar. Vectorising the tap sum would reassociate float addition, and float
/// addition is not associative — the result would be close, and not the same.
class Interpolator {
public:
    /// Number of phase rows in the kernel.
    static constexpr int phase_count = 128;

    /// Taps per phase row.
    static constexpr int tap_count = 4;

    /// Creates the resampler over a loaded table set, which must outlive it.
    explicit Interpolator(const TableSet& tables) : coefficients_(tables.interp_coef()) {}

    /// Reads one sample at a fractional position.
    ///
    /// The window reaches from `i-1` to `i+2`, so the index is clamped to leave room for all four
    /// taps. Near a loop boundary the caller must supply the samples the loop wraps to, not
    /// whatever follows it in the buffer.
    [[nodiscard]] float sample(std::span<const float> buffer, double position) const noexcept;

    /// Reads one sample from a power-of-two ring buffer at an absolute index.
    ///
    /// For a stream generated as it is read rather than held whole — a ping-pong traversal, whose
    /// predictor keeps accumulating in both directions and so never repeats. The window still
    /// reaches from `i-1` to `i+2`, so the caller must have generated two samples ahead.
    [[nodiscard]] float
    sample_ring(std::span<const float> ring, std::int64_t index, double fraction) const noexcept;

    /// Resamples a buffer at a series of fractional positions.
    void resample(std::span<const float> buffer,
                  std::span<const double> positions,
                  std::span<float> destination) const noexcept;

private:
    std::span<const float> coefficients_;
};

/// A wider, band-limiting resampler for the extended mode -- shaped to sound like the module's.
///
/// **What it is for.** The 4-tap kernel above has no band-limiting worth the name: at half the
/// sample rate it is only 14.2 dB down, so a wave read faster than about four times its native rate
/// folds everything above the new Nyquist straight back into the audible band. That is why the
/// engine has a ceiling at 4x at all, and `bigben.mid` -- a Kalimba wave asked for 4.80x -- is the
/// file that put it there. The ceiling is not a property of the hardware: a real SC-55 has none.
/// Removing it needs a kernel that can do the filtering the ceiling was standing in for.
///
/// **Shaped, not merely better.** A textbook Lanczos would be *cleaner* than the module and would
/// change every ordinary note. This one is fitted to the module's own response instead: a windowed
/// sinc at `cutoff` with a tunable Blackman shoulder, the two constants chosen by least squares
/// against the 4-tap bank's averaged magnitude over 0 to a quarter of the sample rate. The fit is
/// **0.165 dB RMS**, and no worse than 0.26 dB anywhere in that range -- so at ordinary playback
/// rates this is the same instrument. What differs is above it: -20.5 dB against -12.5 dB at 0.45
/// of the rate, and -26.2 against -14.2 at Nyquist.
///
/// **The widening is the whole mechanism, and it is why the tap count is not fixed.** Following
/// `spessasynth_core_c`'s `itpSinc`, the kernel argument is scaled by `min(1, 1/ratio)`, which
/// stretches it across more source samples as the read speeds up and drops its cutoff to match the
/// output rate. That only works if the taps are actually there to be summed: the kernel then spans
/// `radius / scale` samples each side, so a 6x read wants about 50 taps and not 8. Capping at 8 --
/// which is what the reference implementation does -- lets the rejection collapse from 26 dB to
/// 6.6 dB at exactly the ratios this exists to serve. With the taps it asks for, rejection at the
/// folding frequency is a flat **-28.3 dB at every ratio measured**, from 1x to 8x.
///
/// The count costs nothing when it is not needed. Past `radius` the kernel is zero, so at 1x the
/// eight-tap and the sixty-four-tap sums are the same number.
class SincInterpolator {
public:
    /// Half-width in source samples at unity ratio: 8 taps.
    static constexpr int radius = 4;

    /// Largest half-width the widening may ask for, bounding a read at 8x.
    static constexpr int max_radius = 32;

    /// Cutoff in cycles per sample, and the window's shoulder. Fitted, not chosen.
    static constexpr double cutoff = 0.29;
    static constexpr double shoulder = 0.20;

    /// Entries per source sample in the kernel table.
    static constexpr int resolution = 1024;

    /// The shared kernel table, built once. `kernel(|d|)` for `|d|` in `[0, radius]` -- the
    /// kernel is even, so one half is stored and `weight` folds the sign away.
    [[nodiscard]] static const SincInterpolator& shared() noexcept;

    /// Reads one sample at a fractional position, band-limited for `ratio`.
    ///
    /// Indices are clamped into the buffer rather than wrapped: a caller that needs loop continuity
    /// supplies a buffer that already carries it, exactly as the 4-tap path requires.
    [[nodiscard]] float
    sample(std::span<const float> buffer, double position, double ratio) const noexcept;

    /// Reads one sample from a power-of-two ring at an absolute index, band-limited for `ratio`.
    [[nodiscard]] float sample_ring(std::span<const float> ring,
                                    std::int64_t index,
                                    double fraction,
                                    double ratio) const noexcept;

private:
    SincInterpolator();

    /// `kernel(|d|)`, linearly interpolated between table entries.
    ///
    /// Folding the sign does two separable things -- it halves the table, and it makes the index
    /// cheaper -- and which of them pays depends entirely on the L1 it runs on. They were priced
    /// apart by sweeping `resolution` on the folded build, which moves the footprint while leaving
    /// the fold, the tap count and the loop identical, so a 64K one-sided table can be compared
    /// against the old 64K two-sided one with the arithmetic as the only difference.
    ///
    /// **The halving** is worth whatever the old table overflowed. The inner loop may read 64
    /// weights per output sample, striding the table rather than walking it, and one half is 32K
    /// where both were 64K. On a core whose L1 falls between those two figures that is the
    /// difference between fitting and missing to L2: on `bigben.mid`, whose 4.80x Kalimba read is
    /// the file this kernel exists for, the same load count went from 2.00G L1 misses to 0.93G and
    /// the render 2.5% faster. Those are d876b71's figures, from the machine it was written on;
    /// they have not been reproduced since, and no machine here has an L1 narrow enough to. The
    /// same commit recorded 77x fewer misses and 8% for the C port, which is not in this tree and
    /// so is not checkable from it -- and since that port's 4-tap kernel is identical to this
    /// engine's, a whole-render figure from it need not be isolating the sinc at all. Treat both as
    /// provenance rather than as measurements this repository can stand behind.
    ///
    /// On a core that already fit the old table it is worth nothing. An M4 Pro P-core has 128K of
    /// L1d, and there a 64K table costs a 32K one +0.3% at 4.80x and +0.1% at 8.00x -- no effect
    /// that survives the noise. Its E-core has 64K, which the old table overflows by eight bytes
    /// and the folded one fits with room to spare, and even there the halving is only -2.9% at
    /// 4.80x. Footprint does not genuinely bite until 256K, which costs 21% at 4.80x on a P-core
    /// and 31% on an E-core. Treat the halving as insurance against a narrow L1, not as a speedup
    /// to expect everywhere.
    ///
    /// **The index** is what pays on every core measured, and it is much the larger half of this
    /// change. `(d + radius) * resolution` cost a small `|d|` up to ten bits of itself to the
    /// addition before the scale could recover them; `|d| * resolution` keeps them, and drops the
    /// add. Held at the old 64K footprint so that only the arithmetic differs, the fold alone is
    /// -8.2% at 4.80x on an M4 Pro P-core and -12.1% on an E-core -- which is the whole of the
    /// -7.9% and -14.7% the two changes together deliver there. `bigben.mid` renders 6.6% faster.
    ///
    /// The mirrored halves likewise agreed only to about 1.5 ulp, their windows having evaluated
    /// `cos` at `t` and at `1-t` rather than at one argument, and now there is one value where
    /// there were two.
    [[nodiscard]] double weight(double d) const noexcept;

    std::vector<double> table_;
};

/// The engine's pan law: an exact 128-entry table, not a computed curve.
///
/// Left reads `T[127 - p]` and right reads `T[p - 1]`, so both land on index 63 at the centre and
/// pan 64 is exactly symmetric at 75/127 ≈ 0.5906 — neither the 0.707 of a constant-power law nor
/// the 0.5 of a linear one. Recovered by sweeping the controller through the engine and reading
/// per-channel RMS, and it reproduces that sweep to within 0.00037.
class PanLaw {
public:
    /// Pan value that sounds centred.
    static constexpr int centre = 64;

    /// Creates the pan law over a loaded table set, which must outlive it.
    explicit PanLaw(const TableSet& tables) : table_(tables.pan()) {}

    /// The left and right gains for a pan position, 0 to 127, with 64 centred.
    ///
    /// Pan 0 is fully left: the right channel is silent, not merely attenuated.
    [[nodiscard]] std::pair<double, double> gains(int pan) const noexcept;

private:
    std::span<const std::uint8_t> table_;
};

} // namespace ts
