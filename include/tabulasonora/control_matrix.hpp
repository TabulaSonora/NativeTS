#pragma once

#include <array>

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
