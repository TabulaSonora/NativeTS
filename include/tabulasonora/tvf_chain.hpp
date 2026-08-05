#pragma once

#include "tabulasonora/envelope_machine.hpp"
#include "tabulasonora/part_modifiers.hpp"
#include "tabulasonora/partial_parameters.hpp"
#include "tabulasonora/segment_envelope.hpp"
#include "tabulasonora/state_variable_filter.hpp"
#include "tabulasonora/table_set.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ts {

/// The filter side of a voice: the cutoff chain that turns static bytes into the two coefficients
/// the state-variable filter needs, and the envelope that moves the cutoff over time.
///
/// The cutoff runs through four stages. A derives the resonance byte, B sums the base with the
/// envelope, C warps the sum and clamps it to a resonance-dependent ceiling, and D produces the Q
/// trim. The warped result then passes through an exponential table before it reaches the filter,
/// which is the step an earlier attempt missed: feeding linear cutoff units to a Chamberlin filter
/// drives `f` to about 1.9, where the state matrix diverges.
///
/// Cutoff is note-independent. There is no key-follow on it; brightness tracks the keyboard through
/// the multisample instead.
/// The engine's anti-zipper smoother for the filter coefficients — `voice_ctrl_ramp_c`.
///
/// The coefficients do not step. `f` and `q` each approach their new value exponentially, closing a
/// fixed fraction of the remaining distance once a millisecond: `step = (target − current) × rate
/// >> 13`, with the rate drawn from a table of `4096 / (2i)`. That makes the fraction `1 / (4i)` and
/// the time constant `4i` milliseconds — the 2 to 40 ms the reverse-engineering notes record.
///
/// Stepping them instead is audible in a way that is easy to miss, because it is not a wrong
/// timbre: refreshing coefficients once per 320-sample control block puts a discontinuity in the
/// filter state every 10 ms, and 100 Hz is not harmonically related to any note, so the splatter
/// lands *between* the harmonics as a broadband floor. Measured on a middle C through Piano 1, that
/// floor sat about 10 dB above the same note rendered through a smoothed filter, and swamped the
/// real signal above about 4 kHz.
///
/// `f` and `q` ramp **independently**, which is why the stability clamp is applied per sample here
/// rather than once when the pair is computed: during a move the two are not the mutually
/// consistent pair a steady-state calculation produces, and a change that raises `q` faster than it
/// lowers `f` can cross a bound neither endpoint crosses.
class CoefficientRamp {
public:
    /// Samples between updates — one millisecond at the engine's rate.
    static constexpr int update_samples = 32;

    /// The rate meets the error thirteen bits down.
    static constexpr int rate_shift = 13;

    /// The rate word for a divider index: `4096 / (2i)`, so the time constant is `4i` milliseconds.
    ///
    /// Index zero is the immediate case and is spelled as the table's own first entry rather than
    /// as a division by nothing.
    [[nodiscard]] static constexpr int rate_word(int index) noexcept
    {
        return index <= 0 ? 4095 : 4096 / (2 * index);
    }

    /// Starts the ramp at a value with no glide.
    ///
    /// A voice beginning mid-sweep starts at the level already reached rather than sliding up to
    /// it, which is what `tvf_env_prep` does by writing the same value to the source and the
    /// target.
    void seed(double value) noexcept
    {
        current_ = value;
        phase_ = 0;
        seeded_ = true;
    }

    [[nodiscard]] bool is_seeded() const noexcept { return seeded_; }

    /// Advances one sample toward a target and returns where it now stands.
    [[nodiscard]] double step(double target, int rate) noexcept
    {
        if (!seeded_) {
            seed(target);
            return current_;
        }
        if (++phase_ >= update_samples) {
            phase_ = 0;
            current_ += (target - current_) * (static_cast<double>(rate) / (1 << rate_shift));
        }
        return current_;
    }

private:
    double current_ = 0.0;
    int phase_ = 0;
    bool seeded_ = false;
};

class TvfChain {
public:
    /// Samples between coefficient refreshes — the 100 Hz control tick.
    static constexpr int control_block_samples = 320;

    /// The divider index the coefficient ramps run at by default.
    ///
    /// The per-partial rate word lives in a voice field this project has not tied back to a tone
    /// table byte, so one index stands for all of them. Index one is the fastest the table offers —
    /// a 4 ms time constant — which is the most conservative choice available: it is the smallest
    /// departure from the unsmoothed behaviour that still removes the discontinuity.
    static constexpr int default_ramp_index = 1;

    /// The envelope's peak and its five stage offsets, in cutoff units relative to that peak.
    struct Offsets {
        int peak = 0;
        std::array<int, 4> segments{};
        int release = 0;
    };

    /// A cutoff envelope and the base cutoff its offsets are added to.
    struct Envelope {
        SegmentEnvelope offsets;
        int base_cutoff = 0;
    };

    /// The pair of coefficients the filter runs on for one control block.
    struct Coefficients {
        double frequency = 0.0;
        double damping = 0.0;
    };

    /// Creates the chain over a loaded table set and the shared machine, both of which must outlive
    /// it.
    TvfChain(const TableSet& tables, const EnvelopeMachine& envelope);

    /// Which filter response a partial takes, or `FilterTap::bypass`.
    ///
    /// Only types 0, 1, 2, 4, 5 and 6 are valid; 3 and 7 bypass. The response comes from bits 10–11
    /// of the type's coefficient word, so the mapping is not the obvious one — type 1 is highpass
    /// and type 2 is bandpass.
    [[nodiscard]] FilterTap tap(int filter_type) const noexcept;

    /// The resonance byte — stage A. Floored at 4.
    [[nodiscard]] static int resonance_byte(const PartialParameters& partial,
                                            int part_resonance = 0x40,
                                            int part_resonance_default = 0x40) noexcept;

    /// Warps a 15-bit cutoff sum and clamps it to the resonance-dependent ceiling — stage C.
    ///
    /// The ceiling at neutral resonance times four is 245,760 — the "fully open" constant an
    /// earlier calibration measured empirically. It is a ceiling, not a saturation of the sum.
    [[nodiscard]] int cutoff_units(double cutoff15, int resonance_byte) const noexcept;

    /// The filter's frequency coefficient `f`.
    ///
    /// Every ramp target passes through the exponential table before it reaches the filter, so what
    /// the filter sees is exponential in the cutoff units. This collapses to `f = 2^(C/16384 -
    /// 15)`, and Chamberlin's `f = 2*sin(pi*fc/fs)` then gives the cutoff in Hz with no fitted
    /// constant at all. The table entries reach 2^18, so the interpolation must be widened before
    /// the shift.
    [[nodiscard]] double frequency_coefficient(int units) const noexcept;

    /// The filter's damping coefficient `q` — stage D.
    ///
    /// This is reciprocal-Q, so the neutral resonance byte 0x40 yields exactly 1.0 and smaller
    /// values are more resonant — effectively `Q = 64 / resonance_byte`.
    [[nodiscard]] double
    damping_coefficient(int units, int resonance_byte, int filter_type) const noexcept;

    /// Both coefficients at once, with `f` clamped to the stability ceiling `q` selects.
    ///
    /// The engine couples the two: `voice_ctrl_ramp_d` ramps `q` and, from the same value, clamps
    /// the `f` that `voice_ctrl_ramp_c` has just written — `f = min(f, g_svf_f_ceil[q_raw >> 8])`.
    /// The ceiling is Chamberlin's own stability bound, `sqrt(q^2 + 4) − q`, so the clamp keeps the
    /// filter out of the region where the loop diverges. Anything that runs the filter must take
    /// its coefficients from here rather than from the two accessors separately.
    ///
    /// The clamp is inert over every cutoff and resonance the engine can reach: `cutoff_units`
    /// already holds `f` below the ceiling for all three `q` branches, though only just — filter
    /// type 6 at resonance byte 4 comes within 0.78%. It is implemented because it is a real stage,
    /// not because it changes a render.
    [[nodiscard]] Coefficients
    coefficients(int units, int resonance_byte, int filter_type) const noexcept;

    /// The shared `g_ramp_exp_tbl`. The pitch ramp decodes its sampler increment from the same
    /// table and the same octave size, so it is exposed here rather than loaded twice.
    [[nodiscard]] std::span<const std::int32_t> ramp_exp() const noexcept { return ramp_exp_; }

    /// The cutoff in Hz. Diagnostic only — the filter never needs it.
    [[nodiscard]] double
    cutoff_hz(double cutoff15, int resonance_byte, int sample_rate = 32000) const noexcept;

    /// The velocity the filter envelope's depth actually responds to.
    ///
    /// Velocity does not reach the depth scaler raw: `block[0x2e]` picks one of sixteen response
    /// curves first. Row 0 is the identity, so the majority of the library is unaffected — Trumpet
    /// selects it and is exact either way. Brass 1 selects row 1, which reads velocity 100 as 71.
    ///
    /// Using raw velocity leaves Brass 1's filter about a third of an octave too open for the whole
    /// note, which measures as +3.5 dB at 4–8 kHz and +6.3 dB above it.
    [[nodiscard]] int effective_velocity(const PartialParameters& partial,
                                         int velocity) const noexcept;

    /// The envelope's peak and its five stage offsets.
    [[nodiscard]] Offsets
    envelope_offsets(const PartialParameters& partial, int key, int velocity) const;

    /// Builds the cutoff envelope for one note, ready to be evaluated at any sample position.
    ///
    /// The running level starts at zero rather than at the release level: all five targets are made
    /// relative to the peak, and the peak itself is folded into the base cutoff instead. TVF
    /// segments are always linear — unlike the TVA, the shape is not data-driven here.
    ///
    /// `modifiers` supplies the part's cutoff offset, which always applies, and its envelope
    /// offsets, which apply only when the partial opts in — see `responds_to_env_modifiers`.
    [[nodiscard]] Envelope create_envelope(const PartialParameters& partial,
                                           int velocity,
                                           int key,
                                           int sample_rate = 32000,
                                           const PartModifiers& modifiers = {}) const;

    /// Whether this partial's filter envelope follows the part's envelope modify offsets.
    ///
    /// Bit 4 of block byte 0x0E. `tvf_compute_env_rates` zeroes its bias outright when the bit is
    /// clear, so on those partials CC#73/75/72 move the amplitude envelope and leave the filter
    /// envelope alone. The amplitude side has no such gate.
    [[nodiscard]] static bool responds_to_env_modifiers(const PartialParameters& partial) noexcept
    {
        return (partial.raw()[0x0E] & 0x10) != 0;
    }

    /// The 15-bit cutoff trajectory over a note, clamped to 15 bits.
    [[nodiscard]] std::vector<double> envelope(const PartialParameters& partial,
                                               int velocity,
                                               int key,
                                               double hold_seconds,
                                               double tail_seconds,
                                               int sample_rate = 32000) const;

    /// Filters a signal in place with a cutoff that moves per control block.
    ///
    /// Coefficients refresh once per control block, matching the engine's 100 Hz rate. The engine
    /// additionally slews them over 2–40 ms through its anti-zipper ramps, which is not modelled.
    ///
    /// The coefficients come from the cutoff's **mean over the block they will serve**, not its
    /// value at the tick: the envelope can cross several segments inside one 10 ms tick, and a
    /// single sample point costs about 1.7% of peak on a piano attack.
    /// `ramp_index` selects the coefficient smoother's rate; a negative value disables the ramp and
    /// restores the stepped behaviour, which is kept so the difference can be measured.
    void apply(std::span<float> signal,
               std::span<const double> cutoff15,
               int filter_type,
               int resonance_byte,
               int block_samples = control_block_samples,
               int ramp_index = default_ramp_index) const noexcept;

private:
    // The env-depth region is addressed by absolute VA in the decompile. Relative to the start of
    // the cached slice (VA 0x1819a2f00) the two sub-tables sit here:
    //   0x1819a3028 - 0x1819a2f00 = 0x128   (depth below neutral)
    //   0x1819a2fa8 - 0x1819a2f00 = 0x0a8   (depth at or above neutral)
    static constexpr int env_depth_low_branch = 0x128;
    static constexpr int env_depth_high_branch = 0x0A8;

    // g_pitch_bias lives inside the pitch-envelope export: 0x1819a2890 - 0x1819a2578 = 0x318.
    static constexpr int pitch_bias_offset = 0x318;

    [[nodiscard]] int env_depth_word(int offset) const;
    [[nodiscard]] int pitch_bias(int magnitude) const;

    /// The damping coefficient before its scaling — the integer the ceiling is indexed by.
    [[nodiscard]] int damping_raw(int units, int resonance_byte, int filter_type) const noexcept;

    /// The stability ceiling `f` must not exceed for a damping the ramp has reached.
    [[nodiscard]] double f_ceiling_for(double damping) const noexcept;

    const EnvelopeMachine* envelope_machine_;
    std::span<const std::uint8_t> env_depth_;
    std::span<const std::uint8_t> pitch_env_;
    std::span<const std::int16_t> env_depth_key_follow_;
    std::span<const std::int16_t> reso_curve_;
    std::span<const std::uint8_t> rate_key_follow_;
    std::span<const std::uint32_t> filter_type_coef_;
    std::span<const std::uint16_t> warp_;
    std::span<const std::uint16_t> ceiling_;
    std::span<const std::uint16_t> q_low_pass_;
    std::span<const std::uint16_t> q_type6_;
    std::span<const std::int32_t> ramp_exp_;
    std::span<const float> f_ceiling_;
    std::span<const std::uint8_t> velocity_sensitivity_;
};

} // namespace ts
