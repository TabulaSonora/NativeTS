#include "tabulasonora/soundfont_bank.hpp"

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

constexpr int curve_linear = 0;
constexpr int curve_concave = 1;
constexpr int curve_switch = 3;

constexpr int cc_modulation = 1;
constexpr int cc_volume = 7;
constexpr int cc_balance = 8;
constexpr int cc_pan = 10;
constexpr int cc_expression = 11;
constexpr int cc_sustain = 64;
constexpr int cc_soft = 67;
constexpr int cc_resonance = 71;
constexpr int cc_release = 72;
constexpr int cc_attack = 73;
constexpr int cc_brightness = 74;
constexpr int cc_decay = 75;
constexpr int cc_vibrato_rate = 76;
constexpr int cc_vibrato_depth = 77;
constexpr int cc_vibrato_delay = 78;
constexpr int cc_reverb = 91;
constexpr int cc_chorus = 93;

constexpr int velocity_source = 2;
constexpr int channel_pressure_source = 13;
constexpr int pitch_wheel_source = 14;
constexpr int pitch_wheel_range_source = 16;

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
    modulators.push_back(make(source(curve_linear, true, false, true, cc_brightness), 0,
                              Gen::initial_filter_fc, 9600));
    modulators.push_back(make(source(curve_linear, true, false, true, cc_resonance), 0,
                              Gen::initial_filter_q, 250));
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
    // Three of these have no counterpart at all in the reader's defaults. The rest of the GS
    // differences are per-partial and cannot live here — the sixteen velocity response curves are
    // selected by `block[0x2e]` per partial, and the half-damper release scaling applies only to
    // the 57 piano tones, so both belong in an instrument's own modulator list. See §6 of the
    // design document.

    // CC#76/77/78 move the tone-common LFO, which is the vibrato LFO here. The amounts are
    // first-order: GS biases table indices rather than scaling a rate directly, so these are the
    // right routes with fitted depths rather than transcribed ones.
    modulators.push_back(make(source(curve_linear, true, false, true, cc_vibrato_rate), 0,
                              Gen::vib_lfo_rate, 600));
    modulators.push_back(make(source(curve_linear, true, false, true, cc_vibrato_depth), 0,
                              Gen::vib_lfo_to_pitch, 300));
    modulators.push_back(make(source(curve_linear, true, false, true, cc_vibrato_delay), 0,
                              Gen::delay_vib_lfo, 2400));

    // CC#64 is deliberately NOT here. Half-damper is a per-tone capability -- 57 piano tones carry
    // it and every other tone quantises the pedal to fully up or fully down -- so a bank-wide
    // default would lengthen the release of the entire library. It is emitted per instrument
    // instead; see `build_bank`.

    return modulators;
}

} // namespace ts::sf2
