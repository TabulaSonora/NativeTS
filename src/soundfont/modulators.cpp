#include "tabulasonora/soundfont_bank.hpp"

#include "modulator_names.hpp"

namespace ts::sf2 {
namespace {

[[nodiscard]] constexpr std::uint16_t source(int curve,
                                             bool bipolar,
                                             bool negative,
                                             bool is_cc,
                                             int index) noexcept
{
    return static_cast<std::uint16_t>((curve << 10) | ((bipolar ? 1 : 0) << 9)
                                      | ((negative ? 1 : 0) << 8) | ((is_cc ? 1 : 0) << 7)
                                      | index);
}

constexpr int attenuation_amount = 960;

[[nodiscard]] Modulator make(std::uint16_t src,
                             std::uint16_t amount_source,
                             Gen destination,
                             int amount)
{
    return Modulator{src, destination, static_cast<std::int16_t>(amount), amount_source, 0};
}

} // namespace

std::uint16_t modulator_source(int curve, bool bipolar, bool negative, bool is_cc,
                               int index) noexcept
{
    return source(curve, bipolar, negative, is_cc, index);
}

std::uint16_t velocity_attenuation_source() noexcept
{
    return source(curve_concave, false, true, false, velocity_source);
}

std::vector<Modulator> default_modulators(bool general_sound_extensions)
{
    std::vector<Modulator> modulators;

    // ── the reader's own defaults, transcribed ───────────────────────────────
    //
    // DMOD replaces the built-in set outright rather than adding to it, so anything still wanted
    // has to be copied across. A DMOD holding only the additions silently removes
    // velocity-to-attenuation and every other default with it, which sounds like a broken bank
    // rather than a missing modulator.
    modulators.push_back(make(source(curve_concave, false, true, false, velocity_source), 0,
                              Gen::initial_attenuation, attenuation_amount));
    modulators.push_back(make(source(curve_linear, false, false, true, cc_modulation), 0,
                              Gen::vib_lfo_to_pitch, 50));
    modulators.push_back(make(source(curve_concave, false, true, true, cc_volume), 0,
                              Gen::initial_attenuation, attenuation_amount));
    modulators.push_back(make(source(curve_linear, false, false, false, channel_pressure_source), 0,
                              Gen::vib_lfo_to_pitch, 50));
    modulators.push_back(make(source(curve_linear, true, false, false, pitch_wheel_source),
                              source(curve_linear, false, false, false, pitch_wheel_range_source),
                              Gen::fine_tune, 12700));
    modulators.push_back(make(source(curve_linear, true, false, true, cc_pan), 0, Gen::pan, 500));
    modulators.push_back(make(source(curve_concave, false, true, true, cc_expression), 0,
                              Gen::initial_attenuation, attenuation_amount));
    modulators.push_back(make(source(curve_linear, false, false, true, cc_reverb), 0,
                              Gen::reverb_effects_send, 200));
    modulators.push_back(make(source(curve_linear, false, false, true, cc_chorus), 0,
                              Gen::chorus_effects_send, 200));

    modulators.push_back(make(source(curve_linear, true, false, true, cc_attack), 0,
                              Gen::attack_vol_env, 6000));
    modulators.push_back(make(source(curve_linear, true, false, true, cc_release), 0,
                              Gen::release_vol_env, 3600));
    modulators.push_back(make(source(curve_linear, true, false, true, cc_decay), 0,
                              Gen::decay_vol_env, 3600));
    if (!general_sound_extensions) {
        // Replaced below in GS mode, not added to: two modulators with the same source and
        // destination in one list have their amounts SUMMED (SF2 §9.5), so leaving this in place
        // alongside the transcribed amount would give CC#74 the sum of the two.
        modulators.push_back(make(source(curve_linear, true, false, true, cc_brightness), 0,
                                  Gen::initial_filter_fc, 9600));
    }
    if (!general_sound_extensions) {
        // CC#71 is one of the reader's defaults but NOT a GS behaviour; see below.
        modulators.push_back(make(source(curve_linear, true, false, true, cc_resonance), 0,
                                  Gen::initial_filter_q, 250));
    }
    modulators.push_back(make(source(curve_switch, false, false, true, cc_soft), 0,
                              Gen::initial_attenuation, 50));
    modulators.push_back(make(source(curve_switch, false, false, true, cc_soft), 0,
                              Gen::initial_filter_fc, -2400));
    modulators.push_back(make(source(curve_linear, true, false, true, cc_balance), 0, Gen::pan,
                              500));

    if (!general_sound_extensions) {
        return modulators;
    }

    // ── what GS has and the default set does not ─────────────────────────────
    //
    // The amounts below are transcribed from the engine's own tables rather than fitted. Each is
    // the excursion a full controller sweep produces from neutral, evaluated at the *median* base
    // index across the mapped library -- a bank-wide default can only carry one number, and the
    // median is the one that is least wrong most often.

    // CC#74 moves the filter cutoff. `PartModifiers::cutoff_offset` is `(cc - 0x40) * 0x100` in
    // 15-bit cutoff units, so a full sweep is 63 * 0x100 = 16128 units. At the median cutoff base
    // byte of 62 that is 391.5 Hz to 5333.3 Hz, or +4522 cents.
    //
    // The engine's ceiling has no SF2 equivalent, so this opens further than the module does on
    // already-bright partials: at base byte 120 the module's sweep is worth nothing at all, because
    // `cutoff_units` has already clamped it to the resonance-dependent maximum.
    //
    // This *replaces* the reader's own CC#74 default rather than joining it. Identical modulators
    // in one list are summed, so emitting both would make a full CC#74 worth 9600 + 4522 cents.
    modulators.push_back(make(source(curve_linear, true, false, true, cc_brightness), 0,
                              Gen::initial_filter_fc, 4522));

    // CC#76/77/78 move the tone-common LFO, which is the vibrato LFO here. They bias table
    // *indices*, which is why the excursion depends on where a patch already sits.

    // Rate: one index step per controller step. The median LFO1 rate index is 52, so a full sweep
    // runs index 52 to 115, which the rate table turns into 5.20 Hz to 16.20 Hz -- 11.00 Hz, or an
    // amount of 1100 in the generator's hundredths-of-a-hertz.
    //
    // `vibLfoRate` is limited to [-1000, 1000], but that limit applies to the *accumulated* value
    // and is applied once at the end, so the honest excursion is what belongs here. Writing 1000
    // instead would rescale the whole curve to be wrong everywhere in order to be right at a rail
    // the reader enforces anyway: at CC#76 = 96, the engine gives about 5.5 Hz, an amount of 1100
    // gives 5.5, and an amount of 1000 gives 5.0.
    modulators.push_back(make(source(curve_linear, true, false, true, cc_vibrato_rate), 0,
                              Gen::vib_lfo_rate, 1100));

    // Depth: two index steps per controller step, into the cents table. The median partial has a
    // depth index of zero -- most of the library carries no vibrato of its own and CC#77 is what
    // adds it -- so a full sweep reaches entry 126, which holds 5570 milli-semitones: 557 cents.
    modulators.push_back(make(source(curve_linear, true, false, true, cc_vibrato_depth), 0,
                              Gen::vib_lfo_to_pitch, 557));

    // Delay: two index steps per controller step, into the delay table. The median delay index is
    // also zero, where the table holds 65535 -- an accumulator that wraps in one control tick, so
    // 10 ms, effectively no delay. A full sweep reaches entry 126, worth 9.10 s. That is +11796
    // timecents. `delayVibLFO` tops out at 5000 timecents, so the *sum* rails at 5.66 s on any
    // partial whose own delay is already long; the amount is still the honest excursion.
    modulators.push_back(make(source(curve_linear, true, false, true, cc_vibrato_delay), 0,
                              Gen::delay_vib_lfo, 11796));

    // CC#71 is deliberately absent, and this is not an omission. The GS resonance byte is written
    // by the CC handler, the NRPN handler, the SysEx handler and the reset path, it reads back over
    // a dump -- and **nothing in the engine ever reads it**. Its patch-level sibling is wired into
    // the resonance path, so this is a route left unconnected rather than a parameter the module
    // has no use for. Keeping the reader's CC#71 default would give the bank a resonance sweep the
    // module does not have.

    // CC#64 is deliberately NOT here. Half-damper is a per-tone capability -- 57 piano tones carry
    // it and every other tone quantises the pedal to fully up or fully down -- so a bank-wide
    // default would lengthen the release of the entire library. It is emitted per instrument
    // instead; see `build_bank`.

    return modulators;
}

} // namespace ts::sf2
