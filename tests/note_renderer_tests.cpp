#include "tabulasonora/note_renderer.hpp"

#include "rom/sha256.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

using namespace ts;
namespace fs = std::filesystem;

namespace {

/// Serialises a render the way the CLI writes it -- interleaved little-endian float32 -- so its
/// digest can be compared against one taken from the reference build's output file.
[[nodiscard]] std::string digest_of(const RenderedNote& note)
{
    Sha256 hash;
    std::array<std::uint8_t, 8> frame{};

    for (std::size_t i = 0; i < note.left.size(); ++i) {
        const float pair[2]{note.left[i], note.right[i]};
        std::memcpy(frame.data(), pair, sizeof(pair));
        hash.update(frame.data(), frame.size());
    }

    return hash.finish_hex();
}

} // namespace

TEST_CASE("a rendered note matches the reference engine bit for bit", "[render][sccore][gate]")
{
    // The Phase 5 gate, and the first end-to-end one: everything from the ROM read through the
    // codec, the resampler, patch resolution, all four envelopes, both LFOs, the filter and the pan
    // law has to compose exactly, or the digest moves.
    //
    // The oracle here is the C# engine itself rather than a reimplementation. At this level that is
    // the point -- what is being checked is that the port assembles the same subsystems the same
    // way, and no independent transcription of a whole synthesiser would be evidence of that.
    const fs::path fixture_path = testdata::repository_root() / "fixtures" / "note_renders.json";
    if (!fs::exists(fixture_path)) {
        SKIP("No render fixture. Generate it with:\n"
             "  python3 tools/dump_note_renders.py <SCCore.dll> fixtures/note_renders.json");
    }

    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer renderer{rom};

    std::ifstream stream{fixture_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);
    REQUIRE(document.at("dllSha256").get<std::string>() == rom.manifest().dll().sha256);

    const auto& cases = document.at("cases");
    REQUIRE(cases.size() >= 100);

    std::size_t compared = 0;
    std::size_t sounding = 0;

    for (const auto& entry : cases) {
        const int program = entry.at("program").get<int>();
        const int note = entry.at("note").get<int>();
        const int velocity = entry.at("velocity").get<int>();
        const double hold = entry.at("hold").get<double>();
        const auto map = static_cast<ToneMap>(entry.at("map").get<int>());

        INFO("program " << program << " note " << note << " velocity " << velocity << " map "
                        << entry.at("map").get<int>());

        const RenderedNote rendered =
            renderer.render_note(program, note, velocity, hold, /*tail_seconds=*/1.8, map);

        REQUIRE(rendered.left.size() == entry.at("frames").get<std::size_t>());
        REQUIRE(rendered.right.size() == rendered.left.size());

        // The literal samples first: when a digest fails these say how it fails.
        const auto& first8 = entry.at("first8");
        for (std::size_t i = 0; i < first8.size(); ++i) {
            const float actual = (i % 2 == 0) ? rendered.left[i / 2] : rendered.right[i / 2];
            REQUIRE(actual == first8[i].get<float>());
        }

        REQUIRE(digest_of(rendered) == entry.at("sha256").get<std::string>());

        if (entry.at("peak").get<double>() > 0.0) {
            ++sounding;
        }
        ++compared;
    }

    CHECK(compared == cases.size());
    // Guard against passing vacuously: a sweep of silent notes would agree perfectly.
    CHECK(sounding == cases.size());
}

TEST_CASE("partials sum rather than average", "[render][sccore]")
{
    // There is no divide-by-count anywhere in the mix path. Averaging would silently halve every
    // two-partial patch relative to a one-partial one, which sounds like a quiet patch rather than
    // like a bug.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer renderer{rom};

    // Find a tone with two sounding partials and one with a single partial.
    std::optional<int> two_partial_program;
    for (int program = 0; program < 128 && !two_partial_program; ++program) {
        const ResolvedTone resolved =
            renderer.directory().resolve_midi(program, 60, 100, ToneMap::sc8820, 0);
        if (resolved.partials.size() == 2) {
            two_partial_program = program;
        }
    }
    REQUIRE(two_partial_program.has_value());

    const RenderedNote note =
        renderer.render_note(*two_partial_program, 60, 100, 0.5, 0.5, ToneMap::sc8820);

    // The mono bus is the plain sum of the partials, so it must be at least as loud as either
    // channel -- an averaged mix could not be.
    const auto peak_of = [](const std::vector<float>& buffer) {
        double peak = 0.0;
        for (float v : buffer) {
            peak = std::max(peak, std::abs(static_cast<double>(v)));
        }
        return peak;
    };

    CHECK(peak_of(note.mono) > 0.0);
    CHECK(peak_of(note.mono) >= peak_of(note.left) - 1e-9);
}

TEST_CASE("an unassigned program renders silence of the right length", "[render][sccore]")
{
    // Not an empty buffer: a caller mixing this into a song still needs the frames.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer renderer{rom};

    // Map 99 does not exist, so nothing resolves.
    const RenderedNote note = renderer.render_note(0, 60, 100, 0.5, 0.5, static_cast<ToneMap>(99));

    CHECK(note.left.size() == 32000);
    CHECK(note.right.size() == 32000);
    CHECK(note.name == "(unassigned)");
    CHECK(std::all_of(note.left.begin(), note.left.end(), [](float v) { return v == 0.0F; }));
}

TEST_CASE("control-rate values are held across their block", "[render]")
{
    // Needs no DLL. The engine updates these at 100 Hz and holds them for the whole block; a
    // per-sample interpolation here would smear every envelope and every LFO.
    const std::vector<double> ticks{1.0, 2.0, 3.0};
    const std::vector<double> expanded = NoteRenderer::expand(ticks, 800);

    REQUIRE(expanded.size() == 800);
    for (std::size_t i = 0; i < 320; ++i) {
        REQUIRE(expanded[i] == 1.0);
    }
    for (std::size_t i = 320; i < 640; ++i) {
        REQUIRE(expanded[i] == 2.0);
    }
    // Past the last tick the final value is held rather than read off the end.
    for (std::size_t i = 640; i < 800; ++i) {
        REQUIRE(expanded[i] == 3.0);
    }

    CHECK(NoteRenderer::expand({}, 4).size() == 4);
    CHECK(NoteRenderer::expand(ticks, 0).empty());
}

TEST_CASE("a drum hit's envelope rate key comes from the coarse-pitch plane", "[render]")
{
    // Not 60, and not the MIDI note. The NRPN moves this index a whole semitone per step where the
    // same step moves the pitch plane by two, so the offset is added rather than read back.
    const DrumKey key{.tone = 100, .level = 127, .pitch = 55, .group = 0, .pan = 64};

    CHECK(NoteRenderer::envelope_rate_key(key, 0) == 55);
    CHECK(NoteRenderer::envelope_rate_key(key, 5) == 60);
    CHECK(NoteRenderer::envelope_rate_key(key, -5) == 50);

    // Clamped into the key range rather than wrapping.
    CHECK(NoteRenderer::envelope_rate_key(key, 200) == 0x7F);
    CHECK(NoteRenderer::envelope_rate_key(key, -200) == 0);
}
