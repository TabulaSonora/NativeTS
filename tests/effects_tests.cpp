#include "tabulasonora/send_effects.hpp"

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
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;
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

TEST_CASE("every send-effect network matches the reference by impulse response", "[effects][gate]")
{
    // The Phase 6 gate: all 26 GS networks -- 8 reverb, 8 chorus, 10 delay -- against the reference
    // build's own impulse responses.
    //
    // The peak assertion is the important one. Twelve of these comparisons were once green upstream
    // while testing nothing at all: the fixture windows were shorter than the delays, so both sides
    // were silent and agreed perfectly. This port nearly repeated it -- delay type 2 does not speak
    // until sample 67,840, so the 48,000-sample window the reverbs use would have made it silent.
    const fs::path fixture_path = testdata::repository_root() / "fixtures" / "effects.json";
    if (!fs::exists(fixture_path)) {
        SKIP("No effects fixture. Generate it with:\n"
             "  python3 tools/dump_effects.py fixtures/effects.json");
    }

    std::ifstream stream{fixture_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);

    const auto& cases = document.at("cases");
    REQUIRE(cases.size() == 26);

    std::size_t compared = 0;
    for (const auto& entry : cases) {
        const auto kind = entry.at("kind").get<std::string>();
        const int type = entry.at("type").get<int>();
        const int samples = entry.at("samples").get<int>();

        INFO(kind << " type " << type);
        const Response response = impulse(kind, type, samples);

        // Non-vacuity first: a silent network agrees with anything.
        const double peak = response.peak();
        REQUIRE(peak > 0.0);
        REQUIRE_THAT(peak, WithinAbs(entry.at("peak").get<double>(), 1e-12));

        // Where it starts speaking, which is the most diagnosable number when a digest moves.
        REQUIRE(response.first_sounding_index()
                == entry.at("firstSoundingIndex").get<std::ptrdiff_t>());

        const auto& last8 = entry.at("last8");
        for (std::size_t i = 0; i < last8.size(); ++i) {
            const std::size_t at = response.left.size() * 2 - last8.size() + i;
            const float actual = (at % 2 == 0) ? response.left[at / 2] : response.right[at / 2];
            REQUIRE(actual == last8[i].get<float>());
        }

        REQUIRE(response.digest() == entry.at("sha256").get<std::string>());
        ++compared;
    }

    CHECK(compared == 26);
}

TEST_CASE("the presets carry all 26 networks", "[effects]")
{
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

TEST_CASE("an out-of-range effect type is refused", "[effects]")
{
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
