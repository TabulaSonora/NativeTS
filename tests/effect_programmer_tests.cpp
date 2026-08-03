#include "tabulasonora/effect_programmer.hpp"

#include "tabulasonora/rom_image.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;

namespace {

/// Computes the presets straight from the DLL, skipping the test when it is absent.
[[nodiscard]] EffectPresets computed()
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    return EffectProgrammer::compute(rom);
}

[[nodiscard]] std::vector<int> tank_taps(const ReverbTank& tank)
{
    return {tank.taps.tap10,
            tank.taps.tap14,
            tank.taps.tap18,
            tank.taps.tap1c,
            tank.taps.tap20,
            tank.taps.tap24,
            tank.taps.tap28,
            tank.taps.tap2c};
}

} // namespace

// The pinned numbers below were read out of a running SCCore.dll on Windows by the C# project's
// EffectHarvester -- they are the values the retired presets.json carried. The computation was
// verified against a full harvest bit for bit, all eight reverb macros, all eight chorus macros and
// both GM defaults, before that file was dropped; these assertions hold the spine of that
// comparison so a regression in any table offset or conversion law trips loudly.

TEST_CASE("the computed defaults are the power-on macros", "[effects][programmer][sccore]")
{
    const EffectPresets presets = computed();

    // The engine boots with Hall2 and Chorus3 selected, so the GM defaults are those types
    // verbatim -- confirmed by the harvest, where the no-macro capture equals the type's.
    const ReverbPreset& hall2 = presets.reverb().types[4];
    CHECK(presets.reverb().defaults.injection_tap == hall2.injection_tap);
    CHECK(tank_taps(presets.reverb().defaults.tank_a) == tank_taps(hall2.tank_a));
    CHECK_THAT(presets.reverb().defaults.gain_feedback, WithinAbs(hall2.gain_feedback, 0.0));

    const ChorusPreset& chorus3 = presets.chorus().types[2];
    CHECK(presets.chorus().defaults.lfo_increment == chorus3.lfo_increment);
    CHECK(presets.chorus().defaults.tap1_base == chorus3.tap1_base);
    CHECK_THAT(presets.chorus().defaults.feedback, WithinAbs(chorus3.feedback, 0.0));
}

TEST_CASE("Hall2 matches the live-engine harvest", "[effects][programmer][sccore]")
{
    const EffectPresets presets = computed();
    const ReverbPreset& hall2 = presets.reverb().types[4];

    CHECK(tank_taps(hall2.tank_a)
          == std::vector<int>{10391, 12618, 13570, 16301, 12194, 10947, 15782, 14252});
    CHECK(tank_taps(hall2.tank_b)
          == std::vector<int>{17163, 19637, 20548, 22537, 17781, 19166, 20932, 22282});
    CHECK_THAT(hall2.tank_a.coef_a, WithinAbs(0.484375, 0.0));
    CHECK_THAT(hall2.tank_a.coef_b, WithinAbs(-0.28125, 0.0));

    // The first diffuser writes at the ring base itself; each later stage writes one past the
    // previous read.
    CHECK(hall2.diffusers[0].write_tap == 8192);
    CHECK(hall2.diffusers[0].read_tap == 9118);
    CHECK_THAT(hall2.diffusers[0].coef_a, WithinAbs(-0.5, 0.0));
    CHECK_THAT(hall2.diffusers[0].coef_b, WithinAbs(0.5, 0.0));
    CHECK(hall2.diffusers[1].write_tap == 9119);

    CHECK(hall2.tank_allpasses.a0.write_tap == 9616);
    CHECK(hall2.tank_allpasses.b1.read_tap == 20547);

    CHECK(hall2.injection_tap == 4096);
    CHECK_THAT(hall2.damp_feedback, WithinAbs(static_cast<double>(1e-05F), 0.0));
    CHECK_THAT(hall2.damp_input, WithinAbs(1.0, 0.0));
    CHECK_THAT(hall2.gain_input, WithinAbs(0.125, 0.0));
    CHECK_THAT(hall2.gain_injection, WithinAbs(1.0, 0.0));
    CHECK_THAT(hall2.gain_feedback, WithinAbs(0.87890625, 0.0));
    CHECK_THAT(hall2.gain_output, WithinAbs(1.0, 0.0));
}

TEST_CASE("the computed laws match the harvest", "[effects][programmer][sccore]")
{
    const EffectPresets presets = computed();

    // Room1: pre-LPF 3 selects the damping pair, and its time byte of 80 prices the feedback.
    const ReverbPreset& room1 = presets.reverb().types[0];
    CHECK_THAT(room1.damp_feedback, WithinAbs(0.375, 0.0));
    CHECK_THAT(room1.damp_input, WithinAbs(0.609375, 0.0));
    CHECK_THAT(room1.gain_feedback, WithinAbs(1.09765625, 0.0));
    CHECK_THAT(room1.tank_a.coef_a, WithinAbs(0.25, 0.0));
    CHECK_THAT(room1.tank_a.coef_b, WithinAbs(-0.40625, 0.0));

    // The two delay characters price feedback from the delay-feedback byte instead, and are the
    // only ones whose tank taps the reverb time moves.
    CHECK_THAT(presets.reverb().types[6].gain_feedback, WithinAbs(0.625, 0.0));
    CHECK_THAT(presets.reverb().types[7].gain_feedback, WithinAbs(0.5, 0.0));

    // Chorus1: rate 3, depth 5, delay 112, no feedback.
    const ChorusPreset& chorus1 = presets.chorus().types[0];
    CHECK(chorus1.lfo_increment == 192);
    CHECK(chorus1.tap1_depth == 240);
    CHECK(chorus1.tap1_base == 2752512);
    CHECK(chorus1.tap2_base == chorus1.tap1_base);
    CHECK_THAT(chorus1.feedback, WithinAbs(static_cast<double>(1e-05F), 0.0));
    CHECK_THAT(chorus1.lpf_a, WithinAbs(static_cast<double>(1e-05F), 0.0));
    CHECK_THAT(chorus1.lpf_b, WithinAbs(1.0, 0.0));
    CHECK_THAT(chorus1.gain_write, WithinAbs(1.0, 0.0));
    CHECK_THAT(chorus1.gain_tap, WithinAbs(1.0, 0.0));

    // FeedbackChorus carries the strongest feedback of the straight choruses, and the two short
    // delays run their LFO stopped.
    CHECK_THAT(presets.chorus().types[4].feedback, WithinAbs(0.5, 0.0));
    CHECK(presets.chorus().types[6].lfo_increment == 0);
    CHECK(presets.chorus().types[7].lfo_increment == 0);

    // The delay rows come straight out of the preset table, already checked by a known row.
    CHECK(presets.delay().raw_presets.size() == 10);
    CHECK(presets.delay().raw_presets[0][1] == 97);
    CHECK(presets.delay().type_names.size() == 10);
    CHECK(presets.delay().time_milliseconds.size() == 115);
    CHECK(presets.delay().ratio_percent.size() == 120);
}

TEST_CASE("the computed set matches a baked harvest when one is present",
          "[effects][programmer][sccore]")
{
    // Anyone still holding a presets.json from the live-engine harvest can pin the whole
    // computation against it: every field of every type must agree exactly.
    const char* baked = std::getenv("TS_PRESETS_ORACLE");
    if (baked == nullptr || *baked == '\0') {
        SKIP("No baked presets.json to compare against. Set TS_PRESETS_ORACLE to one.");
    }

    std::ifstream stream{baked};
    if (!stream) {
        SKIP("TS_PRESETS_ORACLE does not name a readable file.");
    }

    const EffectPresets harvested = EffectPresets::parse(
        std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}});
    const EffectPresets set = computed();

    const auto same_allpass = [](const AllpassStage& a, const AllpassStage& b) {
        CHECK(a.write_tap == b.write_tap);
        CHECK(a.read_tap == b.read_tap);
        CHECK_THAT(a.coef_a, WithinAbs(b.coef_a, 0.0));
        CHECK_THAT(a.coef_b, WithinAbs(b.coef_b, 0.0));
    };

    const auto same_reverb = [&](const ReverbPreset& a, const ReverbPreset& b) {
        for (std::size_t i = 0; i < a.diffusers.size(); ++i) {
            same_allpass(a.diffusers[i], b.diffusers[i]);
        }
        same_allpass(a.tank_allpasses.a0, b.tank_allpasses.a0);
        same_allpass(a.tank_allpasses.a1, b.tank_allpasses.a1);
        same_allpass(a.tank_allpasses.b0, b.tank_allpasses.b0);
        same_allpass(a.tank_allpasses.b1, b.tank_allpasses.b1);
        CHECK(tank_taps(a.tank_a) == tank_taps(b.tank_a));
        CHECK(tank_taps(a.tank_b) == tank_taps(b.tank_b));
        CHECK_THAT(a.tank_a.coef_a, WithinAbs(b.tank_a.coef_a, 0.0));
        CHECK_THAT(a.tank_a.coef_b, WithinAbs(b.tank_a.coef_b, 0.0));
        CHECK_THAT(a.tank_b.coef_a, WithinAbs(b.tank_b.coef_a, 0.0));
        CHECK_THAT(a.tank_b.coef_b, WithinAbs(b.tank_b.coef_b, 0.0));
        CHECK(a.injection_tap == b.injection_tap);
        CHECK_THAT(a.damp_feedback, WithinAbs(b.damp_feedback, 0.0));
        CHECK_THAT(a.damp_input, WithinAbs(b.damp_input, 0.0));
        CHECK_THAT(a.gain_input, WithinAbs(b.gain_input, 0.0));
        CHECK_THAT(a.gain_injection, WithinAbs(b.gain_injection, 0.0));
        CHECK_THAT(a.gain_feedback, WithinAbs(b.gain_feedback, 0.0));
        CHECK_THAT(a.gain_output, WithinAbs(b.gain_output, 0.0));
    };

    const auto same_chorus = [](const ChorusPreset& a, const ChorusPreset& b) {
        CHECK(a.lfo_increment == b.lfo_increment);
        CHECK(a.tap1_depth == b.tap1_depth);
        CHECK(a.tap1_base == b.tap1_base);
        CHECK(a.tap2_depth == b.tap2_depth);
        CHECK(a.tap2_base == b.tap2_base);
        CHECK_THAT(a.lpf_a, WithinAbs(b.lpf_a, 0.0));
        CHECK_THAT(a.lpf_b, WithinAbs(b.lpf_b, 0.0));
        CHECK_THAT(a.feedback, WithinAbs(b.feedback, 0.0));
        CHECK_THAT(a.gain_write, WithinAbs(b.gain_write, 0.0));
        CHECK_THAT(a.gain_tap, WithinAbs(b.gain_tap, 0.0));
    };

    REQUIRE(harvested.reverb().types.size() == set.reverb().types.size());
    for (std::size_t t = 0; t < harvested.reverb().types.size(); ++t) {
        same_reverb(harvested.reverb().types[t], set.reverb().types[t]);
    }
    same_reverb(harvested.reverb().defaults, set.reverb().defaults);

    REQUIRE(harvested.chorus().types.size() == set.chorus().types.size());
    for (std::size_t t = 0; t < harvested.chorus().types.size(); ++t) {
        same_chorus(harvested.chorus().types[t], set.chorus().types[t]);
    }
    same_chorus(harvested.chorus().defaults, set.chorus().defaults);

    CHECK(harvested.delay().raw_presets == set.delay().raw_presets);
    CHECK(harvested.reverb().type_names == set.reverb().type_names);
    CHECK(harvested.chorus().type_names == set.chorus().type_names);
    CHECK(harvested.delay().type_names == set.delay().type_names);
    CHECK(harvested.delay().time_milliseconds == set.delay().time_milliseconds);
    CHECK(harvested.delay().ratio_percent == set.delay().ratio_percent);
}

TEST_CASE("the EQ shelves are unity at 0 dB", "[effects][programmer][sccore]")
{
    const EffectPresets presets = computed();
    REQUIRE(presets.has_eq());

    // 0x40 is the flat setting, twelve steps up from the 0x34 floor. A one-pole shelf whose
    // numerator equals its denominator is unity at every frequency, so the flat row has to read
    // {1, -a, a} exactly -- and it is the check that the three stored coefficients were taken in
    // the right order, because no other assignment of them produces that identity.
    for (int frequency = 0; frequency < 2; ++frequency) {
        INFO("frequency index " << frequency);

        const EqBand& low = presets.eq().low_band(frequency, 0x40);
        CHECK_THAT(low.b0, WithinAbs(1.0, 1e-9));
        CHECK_THAT(low.b1, WithinAbs(-low.a1, 1e-9));
        CHECK(low.a1 > 0.0);

        const EqBand& high = presets.eq().high_band(frequency, 0x40);
        CHECK_THAT(high.b0, WithinAbs(1.0, 1e-9));
        CHECK_THAT(high.b1, WithinAbs(-high.a1, 1e-9));
        CHECK(high.a1 > 0.0);
    }

    // The pole sets the corner, and the second setting of each band has to be the higher one.
    //
    // Only the ordering is asserted, not the frequency. Treating the stored pole as a plain
    // one-pole -3 dB point puts the low band at 225 and 426 Hz against the module's advertised 200
    // and 400, which looks like a match -- and then puts the high band's second setting at 11 kHz
    // against an advertised 6. So that reading is wrong even though half of it agrees, and what
    // Roland means by the printed frequency for a shelf is not pinned down here. The poles
    // themselves are facts; the Hz are not.
    for (int band = 0; band < 2; ++band) {
        const bool low = band == 0;
        const double first =
            low ? presets.eq().low_band(0, 0x40).a1 : presets.eq().high_band(0, 0x40).a1;
        const double second =
            low ? presets.eq().low_band(1, 0x40).a1 : presets.eq().high_band(1, 0x40).a1;
        INFO((low ? "low" : "high") << " poles " << first << " then " << second);

        // A smaller pole is a higher corner, so setting 1 must have the smaller one.
        CHECK(second < first);
    }

    // Gain is monotonic in the setting: b0 is the shelf's far-side gain, so it rises across the
    // whole -12..+12 dB span rather than only near the middle.
    for (int frequency = 0; frequency < 2; ++frequency) {
        CHECK(presets.eq().low_band(frequency, 0x34).b0
              < presets.eq().low_band(frequency, 0x40).b0);
        CHECK(presets.eq().low_band(frequency, 0x40).b0
              < presets.eq().low_band(frequency, 0x4C).b0);
        CHECK(presets.eq().high_band(frequency, 0x34).b0
              < presets.eq().high_band(frequency, 0x40).b0);
        CHECK(presets.eq().high_band(frequency, 0x40).b0
              < presets.eq().high_band(frequency, 0x4C).b0);
    }

    // The engine clamps rather than reading past the table, and so does this.
    CHECK(presets.eq().low_band(0, 0x00).b0 == presets.eq().low_band(0, 0x34).b0);
    CHECK(presets.eq().low_band(0, 0x7F).b0 == presets.eq().low_band(0, 0x4C).b0);
    CHECK(presets.eq().low_band(9, 0x40).b0 == presets.eq().low_band(1, 0x40).b0);
}
