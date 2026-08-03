#pragma once

#include <array>
#include <cstdlib>

namespace ts {

/// The GS controller assignment matrix (`40 2x`): six sources, each with eleven depths.
///
/// This is the module's modulation routing — how far the mod wheel bends the pitch, how much
/// aftertouch opens the filter, how deep CC1 drives LFO2, and so on. The engine keeps one
/// eleven-byte block per source starting at `part+0x3fc` and hands the block to
/// `modmatrix_apply_linear` or `modmatrix_apply_bipolar` with the controller's current amount; the
/// result is eleven signed modulation values the voice adds to its own.
///
/// The depths are 0x40-centred, so a source assigned nothing anywhere leaves the voice alone.
struct ControlMatrix {
    /// Where a modulation comes from, in the order `40 2x` addresses them.
    enum class Source {
        modulation = 0,   ///< CC#1, `40 2x 00`-`0A`
        bend = 1,         ///< pitch bend, `40 2x 10`-`1A`
        channel_pressure, ///< channel aftertouch, `40 2x 20`-`2A`
        poly_pressure,    ///< polyphonic aftertouch, `40 2x 30`-`3A`
        cc1,              ///< the part's CC1, `40 2x 40`-`4A`
        cc2,              ///< the part's CC2, `40 2x 50`-`5A`
    };

    /// What a modulation reaches. The index is the low nibble of the SysEx address.
    enum class Destination {
        pitch = 0,  ///< -24…+24 semitones
        tvf_cutoff, ///< -9600…+9600 cents
        amplitude,  ///< -100…+100 %
        lfo1_rate,  ///< -10…+10 Hz
        lfo1_pitch, ///< 0…600 cents
        lfo1_tvf,   ///< 0…2400 cents
        lfo1_tva,   ///< 0…100 %
        lfo2_rate,  ///< -10…+10 Hz
        lfo2_pitch, ///< 0…600 cents
        lfo2_tvf,   ///< 0…2400 cents
        lfo2_tva,   ///< 0…100 %
    };

    static constexpr int source_count = 6;
    static constexpr int destination_count = 11;

    /// The value every depth centres on, and the value of an unassigned route.
    static constexpr int neutral = 0x40;

    /// The mod wheel's LFO1 pitch depth at power-on — the one destination that is not zero or
    /// centred, and the reason a GM file's mod wheel produces vibrato without being told to.
    static constexpr int default_modulation_lfo1_pitch = 0x0A;

    /// `[source][destination]`.
    std::array<std::array<int, destination_count>, source_count> depth{};

    ControlMatrix() noexcept { reset(); }

    /// Returns every route to power-on.
    void reset() noexcept
    {
        for (auto& source : depth) {
            // The three continuous destinations and the two LFO rates are bipolar and centre at
            // 0x40; the six LFO depths are amounts and start at zero.
            source = {neutral, neutral, neutral, neutral, 0, 0, 0, neutral, 0, 0, 0};
        }
        at(Source::modulation, Destination::lfo1_pitch) = default_modulation_lfo1_pitch;

        // Bend's pitch depth is deliberately absent here — see `bend_pitch_lives_in_bend_range`.
    }

    [[nodiscard]] int& at(Source source, Destination destination) noexcept
    {
        return depth[static_cast<std::size_t>(source)][static_cast<std::size_t>(destination)];
    }

    [[nodiscard]] int at(Source source, Destination destination) const noexcept
    {
        return depth[static_cast<std::size_t>(source)][static_cast<std::size_t>(destination)];
    }

    /// What one source contributes, once its depths have been scaled by its current amount.
    ///
    /// Named fields rather than an array, deliberately. The engine's own output is eleven shorts in
    /// an order that is neither the SysEx order nor the storage order — within each LFO group it
    /// runs TVA, TVF, pitch, the reverse of how the messages arrive. Handing consumers an index
    /// would hand them that permutation to get wrong; handing them names retires it here.
    struct Modulation {
        int pitch = 0;      ///< added to the voice's pitch
        int tvf_cutoff = 0; ///< added to the cutoff sum
        int amplitude = 0;  ///< scales the voice's level
        int lfo1_rate = 0;
        int lfo1_pitch = 0;
        int lfo1_tvf = 0;
        int lfo1_tva = 0;
        int lfo2_rate = 0;
        int lfo2_pitch = 0;
        int lfo2_tvf = 0;
        int lfo2_tva = 0;
    };

    /// Scales this source's depths by a controller amount — `modmatrix_apply_linear`.
    ///
    /// Used by every source except bend, which has its own law (`modmatrix_apply_bipolar`, a
    /// per-destination 16-bit scale rather than a shift) and is not this function.
    ///
    /// This reads more simply than the engine's version, and the reason is worth knowing rather
    /// than trusting. The engine permutes **twice**: `sysex_part_control_matrix` writes SysEx
    /// destination 3 to block byte 4 and destination 10 to block byte 11, skipping byte 3
    /// altogether; then `modmatrix_apply_linear` reads block byte 5 into output 6 and byte 7 into
    /// output 4, running each LFO group backwards. Composed, the two cancel — storing by SysEx
    /// index and naming the outputs makes both disappear. The check that this is a real
    /// cancellation and not a coincidence is that the laws then line up with the published ranges:
    /// every destination that comes out bipolar is one the manual documents as ±something, and
    /// every quartered one is documented as a 0-upward amount.
    ///
    /// Three scalings, not one. Pitch takes the product whole; the two continuous destinations and
    /// the LFO rates take it halved; the six LFO depths take it quartered *and* are unipolar — they
    /// are amounts rather than offsets, so they are not measured from 0x40. The halving is written
    /// as a magnitude shift with the sign reapplied, because an arithmetic shift of a negative
    /// rounds toward minus infinity and the engine's does not.
    [[nodiscard]] Modulation applied_linear(Source source, int amount) const noexcept
    {
        const auto& row = depth[static_cast<std::size_t>(source)];

        // A bipolar destination, halved. `(|d| * amount) >> 1` with the sign put back.
        const auto halved = [amount](int value) {
            const int offset = value - neutral;
            const int magnitude = (std::abs(offset) * amount) >> 1;
            return offset < 0 ? -magnitude : magnitude;
        };

        // A unipolar depth, quartered. No 0x40 offset: zero means none.
        const auto quartered = [amount](int value) { return (value * amount) >> 2; };

        return Modulation{
            .pitch = (row[0] - neutral) * amount,
            .tvf_cutoff = halved(row[1]),
            .amplitude = halved(row[2]),
            .lfo1_rate = halved(row[3]),
            .lfo1_pitch = quartered(row[4]),
            .lfo1_tvf = quartered(row[5]),
            .lfo1_tva = quartered(row[6]),
            .lfo2_rate = halved(row[7]),
            .lfo2_pitch = quartered(row[8]),
            .lfo2_tvf = quartered(row[9]),
            .lfo2_tva = quartered(row[10]),
        };
    }

    /// Bend's pitch depth is **not** stored here, and this says so out loud.
    ///
    /// `40 2x 10` (BEND PITCH CONTROL) and RPN 00/00 (pitch bend sensitivity) are not two
    /// parameters that happen to agree — they are one byte, `part+0x408`, written by both handlers
    /// with the same 0–24 semitone clamp. The engine's bend range *is* this matrix cell. Keeping a
    /// second copy here would be a second source of truth for one value, so `Part::bend_range`
    /// owns it and both messages write there.
    static constexpr bool bend_pitch_lives_in_bend_range = true;
};

} // namespace ts
