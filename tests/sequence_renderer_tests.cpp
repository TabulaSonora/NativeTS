#include "tabulasonora/sequence_renderer.hpp"

#include "rom/sha256.hpp"
#include "tabulasonora/wav_writer.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path canyon_path()
{
    const fs::path path = testdata::repository_root() / "testdata" / "canyon.mid";
    if (!fs::exists(path)) {
        SKIP("No testdata/canyon.mid.");
    }
    return path;
}

/// Applies the option flags the fixture records, which are the CLI's own spellings.
[[nodiscard]] RenderOptions options_from(const nlohmann::json& entry)
{
    RenderOptions options;
    options.map = static_cast<ToneMap>(entry.at("map").get<int>());

    const auto& flags = entry.at("flags");
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const auto flag = flags[i].get<std::string>();
        if (flag == "--no-reverb") {
            options.reverb = false;
        } else if (flag == "--no-chorus") {
            options.chorus = false;
        } else if (flag == "--no-delay") {
            options.delay = false;
        } else if (flag == "--volume") {
            options.output_gain = std::stod(flags[++i].get<std::string>());
        } else if (flag == "--end") {
            options.end_seconds = std::stod(flags[++i].get<std::string>());
        }
    }
    return options;
}

} // namespace

TEST_CASE("a whole song renders identically to the reference engine", "[song][sccore][gate]")
{
    // The end-to-end gate, and the one that subsumes every earlier phase: MIDI parsing, patch
    // resolution, four envelopes, both LFOs, the filter, the drum path with its kits and choke
    // groups, three send effects and the final 16-bit quantisation all have to compose exactly.
    //
    // Ten variants because the flags change which code paths run at all -- a render with the
    // reverb disabled never enters the reverb, so a fault there would hide behind the default.
    const fs::path fixture_path = testdata::repository_root() / "fixtures" / "song_renders.json";
    if (!fs::exists(fixture_path)) {
        SKIP("No song fixture. Generate it with:\n"
             "  python3 tools/dump_song_renders.py <SCCore.dll> testdata/canyon.mid "
             "fixtures/song_renders.json");
    }

    const fs::path midi = canyon_path();
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);

    std::ifstream stream{fixture_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);
    REQUIRE(document.at("dllSha256").get<std::string>() == rom.manifest().dll().sha256);

    const auto& cases = document.at("cases");
    REQUIRE(cases.size() == 10);

    // Each variant is a full 123-second render, so the whole set costs about a minute -- fine in a
    // release build and far too slow under a sanitizer. TS_SONG_GATE_VARIANTS trims it: the
    // sanitizer preset sets it to 1, which still walks every buffer index in this file once.
    std::size_t limit = cases.size();
    if (const char* requested = std::getenv("TS_SONG_GATE_VARIANTS")) {
        limit = std::min(limit, static_cast<std::size_t>(std::max(1, std::atoi(requested))));
        WARN("TS_SONG_GATE_VARIANTS=" << requested << ": checking " << limit << " of "
                                      << cases.size() << " render variants.");
    }

    const fs::path scratch = fs::temp_directory_path() / "ts-song-gate.wav";

    std::size_t compared = 0;
    for (const auto& entry : cases) {
        if (compared >= limit) {
            break;
        }
        INFO("map " << entry.at("map").get<int>() << " flags " << entry.at("flags").dump());

        // A fresh renderer per variant: the wave cache is shared but the noise generator is not
        // meant to carry over, and a render must not depend on what was rendered before it.
        NoteRenderer notes{rom};
        SequenceRenderer renderer{notes};

        const RenderResult result = renderer.render_file(midi, options_from(entry));

        REQUIRE(result.note_count == entry.at("notes").get<int>());
        REQUIRE(result.left.size() == entry.at("frames").get<std::size_t>());

        // Non-vacuity: a silent render agrees with anything.
        REQUIRE(result.peak > 0.0F);

        wav::write(scratch, result.left, result.right, result.sample_rate);

        std::ifstream written{scratch, std::ios::binary};
        REQUIRE(written);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{written},
                                              std::istreambuf_iterator<char>{}};

        Sha256 hash;
        hash.update(bytes.data(), bytes.size());
        REQUIRE(hash.finish_hex() == entry.at("sha256").get<std::string>());

        ++compared;
    }

    fs::remove(scratch);
    CHECK(compared == limit);
}

TEST_CASE("solo wins over mute", "[song]")
{
    // Needs no DLL. Once anything is soloed only soloed channels sound, and a mute on a soloed
    // channel does not silence it -- otherwise a mixer's two controls fight each other.
    ChannelMask mask;
    CHECK(mask.is_default());
    CHECK(mask.is_audible(0));
    CHECK(mask.is_audible(15));

    mask.set_muted(3, true);
    CHECK_FALSE(mask.is_default());
    CHECK_FALSE(mask.is_audible(3));
    CHECK(mask.is_audible(4));

    mask.set_soloed(5, true);
    CHECK_FALSE(mask.is_audible(3));
    CHECK_FALSE(mask.is_audible(4));
    CHECK(mask.is_audible(5));

    // A soloed channel that is also muted still sounds.
    mask.set_muted(5, true);
    CHECK(mask.is_audible(5));

    mask.reset();
    CHECK(mask.is_default());
    CHECK(mask.is_audible(3));

    // Out-of-range channels are inaudible rather than an index error.
    CHECK_FALSE(mask.is_audible(-1));
    CHECK_FALSE(mask.is_audible(16));
}

TEST_CASE("the drum ring stretches with the coarse pitch", "[song][sccore]")
{
    // A drum ignores note-off, so the renderer decides how long to run. A fixed window is wrong
    // once a key is pitched down: the sample plays proportionally slower and the hit outlasts it.
    // Measured on the DLL, the splash at NRPN 18h = 24 takes 4.68 s to fall 40 dB against 1.15 s
    // untouched -- four times the nominal ring.
    const DrumKey natural{.tone = 0, .level = 127, .pitch = 60, .group = 0, .pan = 64};
    CHECK_THAT(NoteRenderer::drum_ring_scale(natural), WithinAbs(1.0, 1e-12));

    // Pitched up rings shorter, but the window never shrinks below the nominal one.
    const DrumKey up{.tone = 0, .level = 127, .pitch = 84, .group = 0, .pan = 64};
    CHECK(NoteRenderer::drum_ring_scale(up) == 1.0);

    // Pitched down stretches it.
    const DrumKey down{.tone = 0, .level = 127, .pitch = 36, .group = 0, .pan = 64};
    CHECK(NoteRenderer::drum_ring_scale(down) > 1.9);

    // And the stretch is bounded, or one deep hit would want a buffer of tens of megabytes.
    const DrumKey deepest{.tone = 0, .level = 127, .pitch = -200, .group = 0, .pan = 64};
    CHECK(NoteRenderer::drum_ring_scale(deepest) == NoteRenderer::max_drum_ring_scale);
}

TEST_CASE("16-bit quantisation truncates toward zero", "[song]")
{
    // Needs no DLL. This is the last thing that happens to a render and it was wrong here first
    // time: a C# cast from double to short truncates, it does not round to nearest. Rounding
    // instead moves roughly a quarter of all samples by one LSB.
    const std::vector<float> left{
        0.0F, 0.5F, -0.5F, 1.0F, -1.0F, 2.0F, -2.0F, 1.0F / 32767.0F * 1.9F};
    const std::vector<float> right = left;

    const fs::path path = fs::temp_directory_path() / "ts-quantise.wav";
    wav::write(path, left, right, 32000);

    std::ifstream stream{path, std::ios::binary};
    REQUIRE(stream);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{stream},
                                          std::istreambuf_iterator<char>{}};
    REQUIRE(bytes.size() == 44 + left.size() * 4);

    const auto sample_at = [&bytes](std::size_t frame, int channel) {
        const std::size_t at = 44 + (frame * 4) + (static_cast<std::size_t>(channel) * 2);
        return static_cast<std::int16_t>(bytes[at] | (bytes[at + 1] << 8));
    };

    CHECK(sample_at(0, 0) == 0);
    CHECK(sample_at(1, 0) == 16383);  // 0.5 * 32767 = 16383.5, truncated
    CHECK(sample_at(2, 0) == -16383); // and toward zero on the negative side too
    CHECK(sample_at(3, 0) == 32767);
    CHECK(sample_at(4, 0) == -32767);

    // Past full scale clamps rather than wrapping: a wrapped sample is a click at the opposite
    // polarity, not a loud one.
    CHECK(sample_at(5, 0) == 32767);
    CHECK(sample_at(6, 0) == -32768);

    // 1.9 LSB truncates to 1, not to 2.
    CHECK(sample_at(7, 0) == 1);

    fs::remove(path);
}
