#pragma once

#include "tabulasonora/lfo_engine.hpp"
#include "tabulasonora/pitch_chain.hpp"
#include "tabulasonora/sequence.hpp"
#include "tabulasonora/tva_chain.hpp"

#include <vector>

namespace ts {

/// One of the sixteen parts: the channel state a running engine keeps between events.
///
/// The engine has no MIDI output, so nothing can be read back from it — every piece of channel
/// state has to be tracked here. This is the live counterpart of `PartTimelines`, which records the
/// same values as breakpoints for the offline renderer.
///
/// Volume, expression and master are set through methods rather than left as public fields: each
/// one recomputes the combined scale, and a plain assignment that skipped that would leave the part
/// sounding at its previous level until the next one happened to move.
class Part {
public:
    Part() { recompute(); }

    /// Program in force, from the last program change.
    int program = 0;
    /// Bank select MSB, which carries the variation.
    int bank = 0;
    /// CC#10 pan.
    int pan = sequence_builder::default_pan;
    /// CC#1 modulation.
    int modulation = 0;
    /// CC#64 damper.
    int damper = 0;
    /// CC#91 reverb send.
    int reverb_send = sequence_builder::default_reverb_send;
    /// CC#93 chorus send.
    int chorus_send = sequence_builder::default_chorus_send;
    /// Part delay send, which has no Control Change and arrives only over SysEx.
    int delay_send = 0;
    /// Pitch bend, as a 14-bit value with 8192 centred.
    int bend = 8192;
    /// Bend range in semitones, from RPN 00/00.
    int bend_range = 2;

    int rpn_msb = 0x7F;
    int rpn_lsb = 0x7F;
    int nrpn_msb = 0x7F;
    /// The selected NRPN's LSB, which for the drum parameters is the key number.
    int nrpn_lsb = 0x7F;

    /// Whether a CC#6 data entry commits to the selected NRPN rather than the selected RPN.
    ///
    /// The two share data entry, so the last selection made decides. Tracking this is what keeps a
    /// file's drum NRPNs out of the bend range.
    bool data_entry_is_nrpn = false;

    /// Per-drum-key overrides this part has taken from NRPN.
    DrumKeyOverrides drum_keys;

    /// Notes whose release is waiting for the damper to lift.
    ///
    /// Insertion-ordered rather than a set: notes are released oldest first when the pedal comes
    /// up, which is the order the offline renderer closes them in too.
    std::vector<int> sustained;

    /// CC#5 portamento time; indexes the glide-step table.
    int portamento_time = 0;
    /// Whether CC#65 portamento is on.
    bool portamento_on = false;
    /// Whether CC#126 mono mode is on, which flushes the part's voices at each note-on.
    bool mono = false;

    /// CC#84 portamento control: the key the next note glides from, or -1 when unset.
    ///
    /// One-shot — the engine consumes it at the next note-on and resets the byte, so it glides
    /// exactly one note and does not latch a mode.
    int portamento_control_key = -1;

    /// The key the part last sounded, which portamento glides from, or -1.
    int last_key = -1;

    /// Whether the sostenuto pedal is down. CC#66 is binary — bit 6 only, as the engine reads it.
    bool sostenuto_down = false;

    /// Notes the sostenuto pedal captured — the ones sounding when it went down.
    std::vector<int> sostenuto_captured;

    /// Captured notes whose note-off arrived while the pedal held them.
    std::vector<int> sostenuto_released;

    /// Whether the damper is holding notes on.
    [[nodiscard]] bool damper_down() const noexcept { return damper >= 0x40; }

    /// CC#7 volume.
    [[nodiscard]] int volume() const noexcept { return volume_; }

    void set_volume(int value) noexcept
    {
        volume_ = value;
        recompute();
    }

    /// CC#11 expression.
    [[nodiscard]] int expression() const noexcept { return expression_; }

    void set_expression(int value) noexcept
    {
        expression_ = value;
        recompute();
    }

    /// Master volume, which is global but folds into the same law.
    [[nodiscard]] int master() const noexcept { return master_; }

    void set_master(int value) noexcept
    {
        master_ = value;
        recompute();
    }

    /// The combined volume multiplier, 1.0 with everything at 127.
    [[nodiscard]] double volume_scale() const noexcept { return volume_scale_; }

    /// The mod wheel's contribution to LFO1 pitch depth, in milli-semitones.
    [[nodiscard]] double mod_wheel_depth() const noexcept
    {
        return LfoEngine::mod_wheel_depth(modulation);
    }

    /// The bend offset in milli-semitones.
    [[nodiscard]] double bend_milli_semitones() const noexcept
    {
        return PitchChain::bend_offset_milli_semitones(bend, bend_range);
    }

    /// Returns the part to its power-on state.
    void reset();

    /// Applies CC#121, which resets the controllers a reset message covers.
    void reset_controllers()
    {
        set_expression(sequence_builder::default_expression);
        bend = 8192;
        damper = 0;
        modulation = 0;
    }

private:
    void recompute() noexcept
    {
        volume_scale_ = TvaChain::part_volume_scale(volume_, expression_, master_);
    }

    int volume_ = sequence_builder::default_volume;
    int expression_ = sequence_builder::default_expression;
    int master_ = sequence_builder::default_master;
    double volume_scale_ = 1.0;
};

} // namespace ts
