#include "tabulasonora/send_effects.hpp"

#include "tabulasonora/effect_programmer.hpp"
#include "tabulasonora/equalizer.hpp"
#include "tabulasonora/rom_image.hpp"

#include "rom/sha256.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

namespace {

/// Runs one network's impulse response, exactly as the CLI does.
struct Response {
    std::vector<float> left;
    std::vector<float> right;

    [[nodiscard]] double peak() const noexcept
    {
        double value = 0.0;
        for (std::size_t i = 0; i < left.size(); ++i) {
            value = std::max({value,
                              std::abs(static_cast<double>(left[i])),
                              std::abs(static_cast<double>(right[i]))});
        }
        return value;
    }

    /// Index into the interleaved stream of the first non-zero sample.
    [[nodiscard]] std::ptrdiff_t first_sounding_index() const noexcept
    {
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (left[i] != 0.0F) {
                return static_cast<std::ptrdiff_t>(i * 2);
            }
            if (right[i] != 0.0F) {
                return static_cast<std::ptrdiff_t>(i * 2 + 1);
            }
        }
        return -1;
    }

    /// The digest of the interleaved little-endian stream the CLI writes.
    [[nodiscard]] std::string digest() const
    {
        Sha256 hash;
        std::array<std::uint8_t, 8> frame{};
        for (std::size_t i = 0; i < left.size(); ++i) {
            const float pair[2]{left[i], right[i]};
            std::memcpy(frame.data(), pair, sizeof(pair));
            hash.update(frame.data(), frame.size());
        }
        return hash.finish_hex();
    }
};

[[nodiscard]] Response impulse(const std::string& kind, int type, int samples)
{
    std::vector<float> input(static_cast<std::size_t>(samples), 0.0F);
    input[0] = 1.0F;

    Response response;
    response.left.assign(input.size(), 0.0F);
    response.right.assign(input.size(), 0.0F);

    if (kind == "reverb") {
        Reverb::for_type(type).process(input, response.left, response.right);
    } else if (kind == "chorus") {
        Chorus::for_type(type).process(input, response.left, response.right);
    } else {
        SystemDelay::for_type(type).process(input, response.left, response.right);
    }
    return response;
}

} // namespace

TEST_CASE("every send-effect network matches the module by impulse response", "[effects][gate]")
{
    testdata::require_effect_presets();

    // All 26 GS networks -- 8 reverb, 8 chorus, 10 delay -- against impulse responses taken from
    // the module itself (`tools/dump_effects_dll.py`, which drives the engine's own processors
    // through scdec). An earlier version of this gate compared against the retired C# port and was
    // bit-exact about it; that only ever proved the two agreed, and they agreed on a delay with
    // five separate faults in it. Facing the module instead is what found them.
    //
    // The comparison is a tolerance rather than a digest, because the module computes in float and
    // this engine in double: that difference alone is worth about 3e-5 on a unit impulse, and no
    // amount of correctness will close it. So the ceiling is 5e-5 -- just above the floor measured
    // across all 26 -- and 1e-5, about -100 dB against these peaks, is asserted on the bulk of the
    // samples instead. Both are far below anything a real fault produces: the delay's swapped tap
    // scaling showed up here as 9.9e-1.
    const fs::path directory = testdata::repository_root() / "fixtures" / "effects_dll";
    if (!fs::exists(directory)) {
        SKIP("No module impulse responses. Generate them with:\n"
             "  python3 tools/dump_effects_dll.py --dll <SCCore.dll>");
    }

    constexpr double ceiling = 5e-5;
    constexpr double tolerance = 1e-5;
    constexpr int captured_samples = 48000;

    struct Network {
        const char* kind;
        int count;
    };
    constexpr std::array<Network, 3> networks{{{"reverb", 8}, {"chorus", 8}, {"delay", 10}}};

    std::size_t compared = 0;
    for (const Network& network : networks) {
        for (int type = 0; type < network.count; ++type) {
            INFO(network.kind << " type " << type);
            const fs::path path =
                directory / (std::string{network.kind} + std::to_string(type) + ".f32");
            REQUIRE(fs::exists(path));

            std::ifstream stream{path, std::ios::binary};
            REQUIRE(stream);
            std::vector<float> reference(static_cast<std::size_t>(captured_samples) * 2);
            stream.read(reinterpret_cast<char*>(reference.data()),
                        static_cast<std::streamsize>(reference.size() * sizeof(float)));
            REQUIRE(stream.gcount()
                    == static_cast<std::streamsize>(reference.size() * sizeof(float)));

            const Response response = impulse(network.kind, type, captured_samples);

            // No compensation. This used to scale the delay's reference by `raw[7] / 127.0`, on
            // the belief that the module's level "lives downstream of its processor" and so was
            // absent from the capture. It is not downstream: `fx_chorus_stage_r` multiplies its own
            // outputs by the level register before it writes them, and `dlyir` prints that very
            // register as `gainOut`. The reference always carried the level.
            //
            // What the factor was really doing was cancelling an identical error on this side --
            // `SystemDelay::compile` folded the same `/127` into its tap gains -- so the comparison
            // agreed while both engines rendered 6 dB apart. Both are now `raw[7] / 64.0`, which is
            // what the live register reads, and the two sides are compared as they stand.
            const double scale = 1.0;

            // Non-vacuity first: a silent network agrees with anything, and delay type 2 does not
            // speak until sample 32000.
            double reference_peak = 0.0;
            for (const float sample : reference) {
                reference_peak = std::max(reference_peak, std::abs(static_cast<double>(sample)));
            }
            REQUIRE(reference_peak > 1e-3);
            REQUIRE(response.peak() > 1e-3);

            double worst = 0.0;
            std::size_t worst_at = 0;
            std::size_t within = 0;
            for (std::size_t i = 0; i < static_cast<std::size_t>(captured_samples); ++i) {
                const double ours_left = static_cast<double>(response.left[i]) / scale;
                const double ours_right = static_cast<double>(response.right[i]) / scale;
                const double error_left =
                    std::abs(ours_left - static_cast<double>(reference[i * 2]));
                const double error_right =
                    std::abs(ours_right - static_cast<double>(reference[(i * 2) + 1]));
                const double error = std::max(error_left, error_right);
                if (error > worst) {
                    worst = error;
                    worst_at = i;
                }
                if (error <= tolerance) {
                    ++within;
                }
            }
            const double fraction =
                static_cast<double>(within) / static_cast<double>(captured_samples);
            INFO("worst error " << worst << " at sample " << worst_at << "; "
                                << (fraction * 100.0) << "% within " << tolerance);
            CHECK(worst <= ceiling);
            CHECK(fraction >= 0.95);
            CHECK_THAT(response.peak() / scale, WithinRel(reference_peak, 1e-3));
            ++compared;
        }
    }

    CHECK(compared == 26);
}

TEST_CASE("the presets carry all 26 networks", "[effects]")
{
    testdata::require_effect_presets();

    const EffectPresets& presets = EffectPresets::defaults();

    CHECK(presets.reverb().types.size() == 8);
    CHECK(presets.chorus().types.size() == 8);
    CHECK(presets.delay().raw_presets.size() == 10);

    // The published names, which are how a GS file's macro selection is read by a person.
    REQUIRE(presets.reverb().type_names.size() == 8);
    CHECK(presets.reverb().type_names.front() == "Room1");
    CHECK(presets.reverb().type_names.back() == "PanDelay");
    CHECK(presets.chorus().type_names.front() == "Chorus1");
    CHECK(presets.delay().type_names.front() == "Delay1");

    // The two published conversion tables the delay compiles through.
    CHECK(presets.delay().time_milliseconds.size() == 115);
    CHECK(presets.delay().ratio_percent.size() == 120);
}

TEST_CASE("the DC blocker is a 20 Hz highpass", "[effects]")
{
    // The coefficient 0.99804 is the only float literal between 0.85 and 1.0 anywhere in the
    // binary, which is how it was established that no second design exists. It is needed because
    // the sample codec's predictor is a pure integrator that drifts on every loop pass.
    DcBlocker blocker;

    // A DC input must decay toward zero rather than being passed through.
    double last = 0.0;
    for (int i = 0; i < 32000; ++i) {
        last = blocker.process(1.0);
        REQUIRE(std::isfinite(last));
    }
    CHECK(std::abs(last) < 0.01);

    blocker.reset();
    // The first sample of a step passes almost unchanged: it is a highpass, not a lowpass.
    CHECK_THAT(blocker.process(1.0), WithinAbs(DcBlocker::input_coefficient, 1e-12));
}

TEST_CASE("send gains are linear in the controller", "[effects]")
{
    // Unlike the volume law, which is squared.
    CHECK(Reverb::send_gain(0) == 0.0);
    CHECK_THAT(Reverb::send_gain(127), WithinAbs(ReverbPresets::send_at_full_scale, 1e-12));
    CHECK_THAT(Reverb::send_gain(64),
               WithinAbs(ReverbPresets::send_at_full_scale * 64.0 / 127.0, 1e-12));

    CHECK_THAT(Chorus::send_gain(127), WithinAbs(ChorusPresets::send_at_full_scale, 1e-12));
    CHECK_THAT(SystemDelay::send_gain(127), WithinAbs(DelayPresets::send_at_full_scale, 1e-12));
}

TEST_CASE("a delay preset compiles to tap lengths and gains", "[effects]")
{
    testdata::require_effect_presets();

    // This is where round-half-to-even actually matters: no grid follows it, so a tap really can
    // land one sample either side of where the reference put it.
    const DelayPresets& presets = EffectPresets::defaults().delay();
    REQUIRE(!presets.raw_presets.empty());

    const DelayParameters compiled = SystemDelay::compile(presets.raw_presets[0]);

    CHECK(compiled.centre_samples >= 1);
    CHECK(compiled.left_samples >= 1);
    CHECK(compiled.right_samples >= 1);
    CHECK(compiled.centre_samples < SystemDelay::ring_size);

    // Raw 0-127 maps the feedback to -1..+1, so a neutral 64 is exactly zero.
    const std::array<int, 10> neutral{0, 97, 1, 1, 127, 0, 0, 64, 64, 0};
    CHECK(SystemDelay::compile(neutral).feedback == 0.0);

    const std::array<int, 10> maximum{0, 97, 1, 1, 127, 0, 0, 64, 127, 0};
    CHECK(SystemDelay::compile(maximum).feedback > 0.9);
}

TEST_CASE("the three cross-feeds between the networks carry signal", "[effects]")
{
    testdata::require_effect_presets();

    // Chorus -> reverb and chorus -> delay (`40 01 3F` and `40 01 40`), and delay -> reverb
    // (`40 01 5A`). All three are zero in every stored macro, so the only way to see them is to
    // program them -- which is exactly the position a stream that sends those addresses is in, and
    // `th07_19_user_gm.mid` sends the first of them at 112.
    ChorusPreset preset = EffectPresets::defaults().chorus().types.at(2);
    preset.gain_to_reverb = 112.0 / 128.0;
    preset.gain_to_delay = 64.0 / 128.0;
    Chorus chorus{preset};

    constexpr std::size_t frames = 8192;
    std::vector<float> input(frames, 0.0F);
    for (std::size_t i = 0; i < 64; ++i) {
        input[i] = 1.0F;
    }
    std::vector<float> left(frames), right(frames), to_reverb(frames), to_delay(frames);
    chorus.process(input, left, right, to_reverb, to_delay);

    double feed_peak = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        feed_peak = std::max(feed_peak, std::abs(static_cast<double>(to_reverb[i])));

        // The feed is the mono sum ahead of the return level, and this preset's return level is
        // unity, so the two sides relate exactly.
        const double mono = static_cast<double>(left[i]) + static_cast<double>(right[i]);
        CHECK_THAT(static_cast<double>(to_reverb[i]),
                   WithinAbs(mono * preset.gain_to_reverb, 1e-6));
        CHECK_THAT(static_cast<double>(to_delay[i]), WithinAbs(mono * preset.gain_to_delay, 1e-6));
    }
    CHECK(feed_peak > 1e-3);

    // And the reverb really sums it: the same send bus with a chorus feed added must not render
    // the same as without one. A silent difference here is the fault this whole path was missing.
    std::vector<float> bus(frames, 0.0F);
    bus[0] = 1.0F;
    std::vector<float> bare_left(frames), bare_right(frames);
    std::vector<float> fed_left(frames), fed_right(frames);
    Reverb::for_type(4).process(bus, bare_left, bare_right);
    Reverb::for_type(4).process(bus, to_reverb, {}, fed_left, fed_right);

    double difference = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        difference = std::max(difference,
                              std::abs(static_cast<double>(fed_left[i])
                                       - static_cast<double>(bare_left[i])));
    }
    CHECK(difference > 1e-4);

    // The delay's own feed is the three taps summed with the centre counted once, so it cannot be
    // reconstructed from the stereo pair -- only checked for presence and for tracking the send.
    std::array<int, 10> row{0, 97, 12, 24, 100, 120, 60, 64, 64, 96};
    SystemDelay delay{SystemDelay::compile(row)};
    CHECK_THAT(SystemDelay::compile(row).send_to_reverb, WithinAbs(96.0 / 128.0, 1e-12));

    std::vector<float> delay_left(frames), delay_right(frames), delay_feed(frames);
    delay.process(input, delay_left, delay_right, delay_feed);
    double delay_feed_peak = 0.0;
    for (const float sample : delay_feed) {
        delay_feed_peak = std::max(delay_feed_peak, std::abs(static_cast<double>(sample)));
    }
    CHECK(delay_feed_peak > 1e-3);

    row[9] = 0;
    SystemDelay silent{SystemDelay::compile(row)};
    std::vector<float> silent_feed(frames, 1.0F);
    silent.process(input, delay_left, delay_right, silent_feed);
    for (const float sample : silent_feed) {
        REQUIRE(sample == 0.0F);
    }
}

TEST_CASE("an out-of-range effect type is refused", "[effects]")
{
    testdata::require_effect_presets();

    CHECK_THROWS_AS(Reverb::for_type(8), std::out_of_range);
    CHECK_THROWS_AS(Chorus::for_type(8), std::out_of_range);
    CHECK_THROWS_AS(SystemDelay::for_type(10), std::out_of_range);
    CHECK_THROWS_AS(SystemDelay::for_type(-1), std::out_of_range);

    // No type at all is the power-on default, which is a real preset rather than an error.
    CHECK_NOTHROW(Reverb::for_type(std::nullopt));
    CHECK_NOTHROW(Chorus::for_type(std::nullopt));
}

TEST_CASE("resetting a network returns it to silence", "[effects]")
{
    testdata::require_effect_presets();

    // A stale ring would leak the previous song's tail into the next one.
    Reverb reverb = Reverb::for_type(4);

    std::vector<float> impulse_in(4096, 0.0F);
    impulse_in[0] = 1.0F;
    std::vector<float> left(impulse_in.size());
    std::vector<float> right(impulse_in.size());

    reverb.process(impulse_in, left, right);
    reverb.reset();

    std::vector<float> silence(4096, 0.0F);
    reverb.process(silence, left, right);

    CHECK(std::all_of(left.begin(), left.end(), [](float v) { return v == 0.0F; }));
    CHECK(std::all_of(right.begin(), right.end(), [](float v) { return v == 0.0F; }));
}

TEST_CASE("the equalizer is exactly transparent when flat", "[effects][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    const EffectPresets presets = EffectProgrammer::compute(rom);
    REQUIRE(presets.has_eq());

    // A signal with content at both ends, so a shelf at either end has something to act on.
    const auto make_signal = [](std::size_t count) {
        std::vector<float> signal(count);
        for (std::size_t n = 0; n < count; ++n) {
            const double t = static_cast<double>(n) / 32000.0;
            signal[n] = static_cast<float>((0.4 * std::sin(2.0 * std::numbers::pi * 100.0 * t))
                                           + (0.4 * std::sin(2.0 * std::numbers::pi * 8000.0 * t)));
        }
        return signal;
    };

    const std::vector<float> reference = make_signal(4096);

    SECTION("flat passes the signal through bit for bit")
    {
        Equalizer eq{presets};
        CHECK(eq.available());
        CHECK(eq.is_flat());

        std::vector<float> left = reference;
        std::vector<float> right = reference;
        eq.process(left, right);

        // Not "close to" -- identical. At 0 dB the shelf's numerator and denominator are the same
        // polynomial, so a flat EQ is the identity and must not perturb a single sample.
        CHECK(left == reference);
        CHECK(right == reference);
    }

    SECTION("a boost and a cut move the band they name")
    {
        const auto energy = [&](int address, int value, double frequency) {
            Equalizer eq{presets};
            if (address == 1) {
                eq.set_low_gain(value);
            } else {
                eq.set_high_gain(value);
            }
            // The same helper measures the flat reference, so this only holds off centre.
            CHECK(eq.is_flat() == (value == Equalizer::flat_gain));

            std::vector<float> left(4096);
            std::vector<float> right(4096);
            for (std::size_t n = 0; n < left.size(); ++n) {
                const double t = static_cast<double>(n) / 32000.0;
                left[n] = static_cast<float>(std::sin(2.0 * std::numbers::pi * frequency * t));
                right[n] = left[n];
            }
            eq.process(left, right);

            // Skip the settling transient and measure the steady state.
            double sum = 0.0;
            for (std::size_t n = 1024; n < left.size(); ++n) {
                sum += static_cast<double>(left[n]) * static_cast<double>(left[n]);
            }
            return sum;
        };

        // The low shelf at 100 Hz, well inside its 200 Hz corner.
        const double low_flat = energy(1, 0x40, 100.0);
        CHECK(energy(1, 0x4C, 100.0) > low_flat * 1.5);
        CHECK(energy(1, 0x34, 100.0) < low_flat * 0.7);

        // The high shelf at 8 kHz, well above its 3 kHz corner.
        const double high_flat = energy(3, 0x40, 8000.0);
        CHECK(energy(3, 0x4C, 8000.0) > high_flat * 1.5);
        CHECK(energy(3, 0x34, 8000.0) < high_flat * 0.7);
    }

    SECTION("out-of-range values are ignored rather than clamped")
    {
        Equalizer eq{presets};
        eq.set_low_gain(0x33);
        eq.set_high_gain(0x4D);
        eq.set_low_frequency(2);
        // Every one was rejected, so the block is still exactly as it started.
        CHECK(eq.is_flat());
    }
}
