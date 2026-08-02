#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/tone_generator.hpp"

#include "tabulasonora/wav_writer.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace ts;
using Catch::Matchers::WithinAbs;
namespace fs = std::filesystem;

namespace {

/// Reads the 16-bit samples out of a WAV, skipping the 44-byte header.
[[nodiscard]] std::vector<std::int16_t> read_wav_samples(const fs::path& path)
{
    std::ifstream stream{path, std::ios::binary};
    REQUIRE(stream);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{stream},
                                          std::istreambuf_iterator<char>{}};
    REQUIRE(bytes.size() > 44);

    std::vector<std::int16_t> samples((bytes.size() - 44) / 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::int16_t>(bytes[44 + i * 2] | (bytes[45 + i * 2] << 8));
    }
    return samples;
}

} // namespace

TEST_CASE("the block loop reproduces the reference to within one LSB", "[stream][sccore][gate]")
{
    // The Phase 7 gate, and the only one in this port that is not bit-exact. What the residual is,
    // and why it is left:
    //
    // The reference's ping-pong reader allocates its ring once per voice object and reuses it, so a
    // recycled voice begins with the previous note's samples still in the far end of the ring --
    // which the 4-tap window reads on the very first sample, at index -1. Reproducing that
    // faithfully took the difference from 544,148 bytes to 549. What is left is the same mechanism
    // in rarer cases: it depends on which recycled voice object a note happens to land on, which is
    // an accident of allocation rather than a behaviour.
    //
    // Measured across all four tone maps and both files: 0.006% of samples differ and none by more
    // than one LSB -- isolated samples at about -90 dBFS. The offline path remains bit-exact, so
    // this bound is on the block loop's own arithmetic and nothing upstream of it.
    //
    // Chasing it further would mean replicating an uninitialised-buffer accident exactly. The bound
    // is asserted instead, and it is tight enough that a real regression cannot hide under it.
    const fs::path index_path = testdata::repository_root() / "fixtures" / "stream_renders.json";
    if (!fs::exists(index_path)) {
        SKIP("No stream fixtures. Generate them with:\n"
             "  python3 tools/dump_stream_renders.py <SCCore.dll> fixtures testdata/canyon.mid");
    }

    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);

    std::ifstream stream{index_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);
    const auto& cases = document.at("cases");
    REQUIRE(cases.size() == 4);

    std::size_t limit = cases.size();
    if (const char* requested = std::getenv("TS_SONG_GATE_VARIANTS")) {
        limit = std::min(limit, static_cast<std::size_t>(std::max(1, std::atoi(requested))));
    }

    std::size_t compared = 0;
    for (const auto& entry : cases) {
        if (compared >= limit) {
            break;
        }

        const fs::path midi =
            testdata::repository_root() / "testdata" / entry.at("midi").get<std::string>();
        const fs::path reference =
            testdata::repository_root() / "fixtures" / entry.at("reference").get<std::string>();
        if (!fs::exists(midi) || !fs::exists(reference)) {
            continue;
        }

        INFO(entry.at("midi").get<std::string>() << " map " << entry.at("map").get<int>());

        NoteRenderer notes{rom};
        ToneGeneratorOptions options;
        options.map = static_cast<ToneMap>(entry.at("map").get<int>());

        ToneGenerator generator{notes, options};
        SequencePlayer player = SequencePlayer::from_file(generator, midi);
        const RenderResult result = player.render_to_end();

        // Structure first: a divergence here is a real fault, not a rounding residual.
        REQUIRE(result.note_count == entry.at("notes").get<int>());
        REQUIRE(result.left.size() == entry.at("frames").get<std::size_t>());
        REQUIRE(result.peak > 0.0F);
        REQUIRE_THAT(static_cast<double>(result.peak),
                     WithinAbs(entry.at("peak").get<double>(), 2.0 / 32767.0));

        const fs::path scratch = fs::temp_directory_path() / "ts-stream-gate.wav";
        wav::write(scratch, result.left, result.right, result.sample_rate);

        const std::vector<std::int16_t> expected = read_wav_samples(reference);
        const std::vector<std::int16_t> actual = read_wav_samples(scratch);
        fs::remove(scratch);

        REQUIRE(actual.size() == expected.size());

        std::size_t differing = 0;
        int worst = 0;
        for (std::size_t i = 0; i < actual.size(); ++i) {
            const int difference = std::abs(static_cast<int>(actual[i]) - expected[i]);
            if (difference != 0) {
                ++differing;
                worst = std::max(worst, difference);
            }
        }

        INFO(differing << " of " << actual.size() << " samples differ, worst " << worst << " LSB");

        // No sample may move by more than one step of the 16-bit grid.
        REQUIRE(worst <= 1);

        // And it must stay rare. Measured at 0.0058-0.0069%; 0.02% leaves room for a different
        // machine's libm without admitting anything structural.
        REQUIRE(static_cast<double>(differing) / static_cast<double>(actual.size()) < 0.0002);

        ++compared;
    }

    CHECK(compared == limit);
}

TEST_CASE("the pool steals whole notes, not half of them", "[stream]")
{
    // Needs no DLL. Partials of one note share a note group so that stealing removes a whole note;
    // a surviving partial of a stolen note would keep sounding on its own.
    VoicePool pool;

    std::vector<int> stolen;
    pool.stealing = [&stolen](int index) { stolen.push_back(index); };

    // Fill every slot with two-partial notes.
    for (int note = 0; note < VoicePool::max_voices / 2; ++note) {
        const int group = pool.begin_note_group();
        static_cast<void>(pool.allocate(0, note, 100, group));
        static_cast<void>(pool.allocate(0, note, 100, group));
    }
    REQUIRE(pool.active_count() == VoicePool::max_voices);

    // The next allocation has to steal, and it must take both partials of whichever note it picks.
    const int group = pool.begin_note_group();
    static_cast<void>(pool.allocate(1, 99, 100, group));
    CHECK(stolen.size() == 2);
}

TEST_CASE("a free slot is preferred, then the oldest releasing note", "[stream]")
{
    // The policy itself is an acknowledged approximation -- the engine's own selection rules were
    // never traced -- but it is deterministic, and these are the properties it claims.
    VoicePool pool;

    const Voice first = pool.allocate(0, 60, 100, pool.begin_note_group());
    CHECK(first.index == 0);
    CHECK(first.state() == VoiceState::held);

    // A second note takes a different slot rather than stealing the first.
    const Voice second = pool.allocate(0, 62, 100, pool.begin_note_group());
    CHECK(second.index != first.index);

    CHECK(pool.release(0, 60) == 1);
    CHECK(first.state() == VoiceState::releasing);

    pool.reset();
    CHECK(pool.active_count() == 0);
}

TEST_CASE("a running engine holds a note indefinitely", "[stream][sccore]")
{
    // This is what the offline path cannot express: it has no notion of now, so a note's length has
    // to be known before it is rendered.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    std::vector<float> left(ToneGenerator::block_size);
    std::vector<float> right(ToneGenerator::block_size);

    generator.send_channel(0x90, 60, 100);
    CHECK(generator.note_count() == 1);
    CHECK(generator.active_voices() > 0);

    // Hold it for two seconds without telling the engine anything.
    double peak = 0.0;
    for (int block = 0; block < 2 * ToneGenerator::sample_rate / ToneGenerator::block_size;
         ++block) {
        generator.render(left, right);
        for (float v : left) {
            peak = std::max(peak, std::abs(static_cast<double>(v)));
        }
    }
    CHECK(peak > 0.0);
    CHECK(generator.position() == 2 * ToneGenerator::sample_rate);

    generator.send_channel(0x80, 60, 0);

    // It releases and eventually frees its slot.
    for (int block = 0; block < 10 * ToneGenerator::sample_rate / ToneGenerator::block_size;
         ++block) {
        generator.render(left, right);
        if (generator.active_voices() == 0) {
            break;
        }
    }
    CHECK(generator.active_voices() == 0);
}

TEST_CASE("reset silences the engine and rewinds its clock", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    std::vector<float> left(ToneGenerator::block_size);
    std::vector<float> right(ToneGenerator::block_size);

    generator.send_channel(0x90, 60, 100);
    generator.render(left, right);
    REQUIRE(generator.active_voices() > 0);

    generator.reset();
    CHECK(generator.active_voices() == 0);
    CHECK(generator.position() == 0);
    CHECK(generator.note_count() == 0);

    // Nothing is left ringing.
    generator.render(left, right);
    CHECK(std::all_of(left.begin(), left.end(), [](float v) { return v == 0.0F; }));
}

TEST_CASE("any render length is accepted", "[stream][sccore]")
{
    // Blocks are rendered whole and the remainder carried, because a voice counts its control tick
    // in them. A caller asking for an awkward length must still get continuous audio.
    //
    // Two *fresh* engines rather than one reset in between, and that is not incidental -- see the
    // test below for why a reset engine is not the same as a new one.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ToneGenerator whole_engine{notes};
    whole_engine.send_channel(0x90, 60, 100);

    std::vector<float> whole(1000);
    std::vector<float> whole_right(1000);
    whole_engine.render(whole, whole_right);

    ToneGenerator pieced_engine{notes};
    pieced_engine.send_channel(0x90, 60, 100);

    std::vector<float> pieces(1000);
    std::vector<float> pieces_right(1000);
    std::size_t at = 0;
    for (std::size_t chunk : {7U, 1U, 32U, 33U, 100U, 227U, 600U}) {
        const std::size_t count = std::min(chunk, pieces.size() - at);
        pieced_engine.render(std::span{pieces}.subspan(at, count),
                             std::span{pieces_right}.subspan(at, count));
        at += count;
    }
    REQUIRE(at == pieces.size());

    for (std::size_t i = 0; i < whole.size(); ++i) {
        REQUIRE(pieces[i] == whole[i]);
        REQUIRE(pieces_right[i] == whole_right[i]);
    }
}

TEST_CASE("a reset engine is not bit-identical to a new one", "[stream][sccore]")
{
    // Recorded because it surprised this port and because it is inherited rather than chosen.
    //
    // The reference allocates a voice's ping-pong ring once and reuses it, so a recycled voice
    // starts with the previous note's samples in the far end of the ring -- which the 4-tap window
    // reads at index -1 on the very first sample. Reproducing that is what makes the block loop
    // match the reference at all (see the gate above), and the cost is this: after a reset the
    // voices are back in the spare pool with their rings still populated, so the next note can
    // differ in the last bit from the same note on a brand-new engine.
    //
    // The difference is one LSB on isolated samples. It is pinned rather than fixed because fixing
    // it means diverging from the reference, and asserted as a bound rather than left unstated.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto play = [](ToneGenerator& engine) {
        std::vector<float> left(4000);
        std::vector<float> right(4000);
        engine.send_channel(0x90, 60, 100);
        engine.render(left, right);
        return left;
    };

    ToneGenerator fresh{notes};
    const std::vector<float> first = play(fresh);

    ToneGenerator reused{notes};
    static_cast<void>(play(reused));
    reused.reset();
    const std::vector<float> after_reset = play(reused);

    REQUIRE(first.size() == after_reset.size());

    double worst = 0.0;
    double peak = 0.0;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < first.size(); ++i) {
        const double difference = std::abs(static_cast<double>(first[i] - after_reset[i]));
        if (difference > 0.0) {
            ++differing;
        }
        worst = std::max(worst, difference);
        peak = std::max(peak, std::abs(static_cast<double>(first[i])));
    }

    INFO(differing << " of " << first.size() << " samples differ, worst " << worst
                   << " against a peak of " << peak);

    // Measured at about 8.3e-05 against a peak near 0.1 -- roughly -62 dB relative, on isolated
    // samples. Bounded at a thousandth of full scale, which is far below anything audible and far
    // above the measurement, so this catches a structural regression without tracking noise.
    CHECK(worst < 1e-3);
    CHECK(peak > 0.0);
}

TEST_CASE("a seek replays state but not notes", "[stream][sccore]")
{
    // Program changes, bank selects, controllers and the GS effect selections all arrive, so a seek
    // into the middle of a song sounds the way playing up to that point would.
    const fs::path midi = testdata::repository_root() / "testdata" / "canyon.mid";
    if (!fs::exists(midi)) {
        SKIP("No testdata/canyon.mid.");
    }

    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};
    SequencePlayer player = SequencePlayer::from_file(generator, midi);

    player.seek(30 * ToneGenerator::sample_rate);
    CHECK(player.position() == 30 * ToneGenerator::sample_rate);

    // Nothing is sounding immediately after a seek -- the notes in between were skipped.
    CHECK(generator.active_voices() == 0);

    // But the parts carry the state the file had set by then.
    bool any_program_set = false;
    for (int channel = 0; channel < Sequence::channel_count; ++channel) {
        any_program_set = any_program_set || generator.part(channel).program != 0;
    }
    CHECK(any_program_set);

    // And playing on from there produces sound.
    std::vector<float> left(ToneGenerator::sample_rate);
    std::vector<float> right(ToneGenerator::sample_rate);
    player.render(left, right);

    double peak = 0.0;
    for (float v : left) {
        peak = std::max(peak, std::abs(static_cast<double>(v)));
    }
    CHECK(peak > 0.0);
}
