#include "tabulasonora/control_matrix.hpp"
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
using Catch::Matchers::WithinRel;
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
    // than one LSB -- isolated samples at about -90 dBFS. Every other gate was bit-exact against
    // the same reference, so this bound is on the block loop's own arithmetic and nothing upstream
    // of it.
    //
    // Chasing it further would mean replicating an uninitialised-buffer accident exactly. The bound
    // is asserted instead, and it is tight enough that a real regression cannot hide under it.
    //
    // What this gate is *now*: all four of its cases touch a wave that starts mid-block, and the
    // exact-start decode moved every one of them away from what the archived C# engine produced.
    // `dump_stream_renders.py` still harvests that engine, so a fixture generated from it no longer
    // applies -- the references this gate passes against are this engine's own output, kept as a
    // regression baseline until the DLL-derived gate replaces it. It is not an independent check.
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

    // However many the generator was pointed at. It used to be exactly four, because it was always
    // canyon across four maps; now that the references come from this engine the corpus is whoever
    // ran it, and a fixture with three files in it is not a broken fixture.
    REQUIRE_FALSE(cases.empty());

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

namespace {

/// Builds a GS DT1 with the checksum the engine verifies: address and data folded to a multiple
/// of 128.
[[nodiscard]] std::vector<std::uint8_t> dt1(std::initializer_list<int> address_and_data)
{
    std::vector<std::uint8_t> message{0xF0, 0x41, 0x10, 0x42, 0x12};
    int sum = 0;
    for (int byte : address_and_data) {
        message.push_back(static_cast<std::uint8_t>(byte));
        sum += byte;
    }
    message.push_back(static_cast<std::uint8_t>((0x80 - (sum & 0x7F)) & 0x7F));
    message.push_back(0xF7);
    return message;
}

} // namespace

TEST_CASE("the controller set the engine dispatches is handled", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // CC#94 is the delay send's controller alias.
    generator.send_channel(0xB0, 94, 55);
    CHECK(generator.part(0).delay_send == 55);

    // Bank select LSB is stored; 42 is outside the 1-4 map range and selects nothing.
    generator.send_channel(0xB0, 32, 42);
    CHECK(generator.part(0).bank_lsb == 42);

    // Soft pedal is binary at bit 6.
    generator.send_channel(0xB0, 67, 0x3F);
    CHECK_FALSE(generator.part(0).soft);
    generator.send_channel(0xB0, 67, 0x40);
    CHECK(generator.part(0).soft);

    // The sound controllers land on the shared modify offsets.
    generator.send_channel(0xB0, 71, 0x50);
    generator.send_channel(0xB0, 72, 0x51);
    generator.send_channel(0xB0, 73, 0x52);
    generator.send_channel(0xB0, 74, 0x53);
    generator.send_channel(0xB0, 75, 0x54);
    generator.send_channel(0xB0, 76, 0x55);
    generator.send_channel(0xB0, 77, 0x56);
    generator.send_channel(0xB0, 78, 0x57);
    CHECK(generator.part(0).tvf_resonance == 0x50);
    CHECK(generator.part(0).env_release == 0x51);
    CHECK(generator.part(0).env_attack == 0x52);
    CHECK(generator.part(0).tvf_cutoff == 0x53);
    CHECK(generator.part(0).env_decay == 0x54);
    CHECK(generator.part(0).vibrato_rate == 0x55);
    CHECK(generator.part(0).vibrato_depth == 0x56);
    CHECK(generator.part(0).vibrato_delay == 0x57);
}

TEST_CASE("RPN fine and coarse tune move the part's static tune", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // Coarse tune +2 semitones: RPN 00/02, data entry 0x42.
    generator.send_channel(0xB0, 101, 0);
    generator.send_channel(0xB0, 100, 2);
    generator.send_channel(0xB0, 6, 0x42);
    CHECK(generator.part(0).coarse_tune == 0x42);
    CHECK_THAT(generator.part(0).tune_milli_semitones(), WithinAbs(2000.0, 1e-9));

    // Fine tune fully sharp: +100 cents at 0x3FFF, to 14-bit precision.
    generator.send_channel(0xB0, 100, 1);
    generator.send_channel(0xB0, 6, 0x7F);
    generator.send_channel(0xB0, 38, 0x7F);
    CHECK(generator.part(0).fine_tune == 0x3FFF);

    // The null RPN parks data entry: this commit must reach nothing.
    generator.send_channel(0xB0, 101, 0x7F);
    generator.send_channel(0xB0, 100, 0x7F);
    generator.send_channel(0xB0, 6, 0x10);
    CHECK(generator.part(0).coarse_tune == 0x42);
    CHECK(generator.part(0).bend_range == 2);

    // Reset All Controllers takes only a zero data byte, then parks the selection and lifts the
    // pedals; the tuning stays.
    generator.send_channel(0xB0, 101, 0);
    generator.send_channel(0xB0, 100, 0);
    generator.send_channel(0xB0, 6, 12);
    CHECK(generator.part(0).bend_range == 12);
    generator.send_channel(0xB0, 121, 1);
    CHECK(generator.part(0).bend_range == 12);
    generator.send_channel(0xB0, 121, 0);
    CHECK(generator.part(0).bend_range == 12);
    CHECK(generator.part(0).coarse_tune == 0x42);
    generator.send_channel(0xB0, 6, 3);
    CHECK(generator.part(0).bend_range == 12);
}

TEST_CASE("the NRPN modify set shares bytes with the sound controllers", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    generator.send_channel(0xB0, 99, 0x01);
    generator.send_channel(0xB0, 98, 0x20);
    generator.send_channel(0xB0, 6, 0x30);
    CHECK(generator.part(0).tvf_cutoff == 0x30);

    // `01 21` is the resonance, the byte next door. It reaches the filter through `modifiers()`
    // like the other seven -- a route that used to stop at the part on the strength of a decompile
    // search that could not see the reads.
    generator.send_channel(0xB0, 99, 0x01);
    generator.send_channel(0xB0, 98, 0x21);
    generator.send_channel(0xB0, 6, 0x2A);
    CHECK(generator.part(0).tvf_resonance == 0x2A);
    CHECK(generator.part(0).modifiers().tvf_resonance == 0x2A);
    CHECK_FALSE(generator.part(0).modifiers().is_neutral());

    // The drum planes only land on a rhythm part.
    generator.send_channel(0xB0, 99, 0x1A);
    generator.send_channel(0xB0, 98, 46);
    generator.send_channel(0xB0, 6, 0x20);
    CHECK_FALSE(generator.part(0).drum_keys.level(46).has_value());

    generator.send_channel(0xB9, 99, 0x1A);
    generator.send_channel(0xB9, 98, 46);
    generator.send_channel(0xB9, 6, 0x20);
    CHECK(generator.part(9).drum_keys.level(46) == 0x20);
}

TEST_CASE("GS part parameters arrive over SysEx", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // Block 1 is channel 0. Part level, then the panpot's true zero -- RND, which CC#10 cannot
    // reach.
    generator.send_sysex(dt1({0x40, 0x11, 0x19, 90}));
    CHECK(generator.part(0).volume() == 90);
    generator.send_sysex(dt1({0x40, 0x11, 0x1C, 0}));
    CHECK(generator.part(0).pan == 0);

    // A wrong checksum drops the message, as the engine's receive parser does.
    std::vector<std::uint8_t> bad = dt1({0x40, 0x11, 0x19, 41});
    bad[bad.size() - 2] = (bad[bad.size() - 2] + 1) & 0x7F;
    generator.send_sysex(bad);
    CHECK(generator.part(0).volume() == 90);

    // Key shift is clamped to the engine's 0x28-0x58.
    generator.send_sysex(dt1({0x40, 0x11, 0x16, 0x10}));
    CHECK(generator.part(0).key_shift == 0x28);

    // Scale tuning arrives as a run of twelve.
    generator.send_sysex(dt1({0x40,
                              0x11,
                              0x40,
                              0x40,
                              0x42,
                              0x40,
                              0x40,
                              0x40,
                              0x40,
                              0x40,
                              0x40,
                              0x40,
                              0x40,
                              0x40,
                              0x40}));
    CHECK(generator.part(0).scale_tuning[1] == 0x42);
    CHECK_THAT(generator.part(0).scale_offset_milli_semitones(61), WithinAbs(20.0, 1e-9));

    // Rx channel off detaches the part.
    generator.send_sysex(dt1({0x40, 0x11, 0x02, 0x10}));
    generator.send_channel(0x90, 60, 100);
    CHECK(generator.note_count() == 0);

    // Master volume and the GS reset, which returns everything to power-on.
    generator.send_sysex(dt1({0x40, 0x00, 0x04, 50}));
    CHECK(generator.part(1).master() == 50);
    generator.send_sysex(dt1({0x40, 0x00, 0x7F, 0x00}));
    CHECK(generator.part(0).volume() == 100);
    CHECK(generator.part(0).pan == 64);
    CHECK(generator.part(0).master() == 127);
    generator.send_channel(0x90, 60, 100);
    CHECK(generator.note_count() == 1);
}

TEST_CASE("drum setup SysEx writes the per-key planes", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // 41 x2 kk is the level plane; it lands on the port's rhythm part.
    generator.send_sysex(dt1({0x41, 0x02, 40, 100, 101, 102}));
    CHECK(generator.part(9).drum_keys.level(40) == 100);
    CHECK(generator.part(9).drum_keys.level(41) == 101);
    CHECK(generator.part(9).drum_keys.level(42) == 102);
    CHECK_FALSE(generator.part(0).drum_keys.level(40).has_value());

    // Assign group clamps to the engine's 1-4.
    generator.send_sysex(dt1({0x41, 0x03, 40, 7}));
    CHECK(generator.part(9).drum_keys.group(40) == 4);

    // The map nibble is part of the address: a MAP2 write (41 1x) lands in the buffer parts on
    // MAP2 read, and the default rhythm part is on MAP1, so nothing here may move. intro-4.mid
    // is the real case -- its whole drum setup, Rx switches included, is written to MAP2 while
    // its rhythm part never leaves MAP1, and the module plays it as if the block were not there.
    generator.send_sysex(dt1({0x41, 0x12, 50, 99}));
    CHECK_FALSE(generator.part(9).drum_keys.level(50).has_value());
    generator.send_sysex(dt1({0x41, 0x18, 38, 0x00}));
    generator.send_channel(0x99, 38, 100);
    CHECK(generator.note_count() == 1); // the MAP2 Rx Note On revocation does not reach MAP1

    // Reassigned to MAP2 (use-for-rhythm = 2), the part hears MAP2 writes instead.
    generator.send_sysex(dt1({0x40, 0x10, 0x15, 0x02}));
    generator.send_sysex(dt1({0x41, 0x12, 50, 99}));
    CHECK(generator.part(9).drum_keys.level(50) == 99);
    generator.send_sysex(dt1({0x41, 0x02, 51, 98}));
    CHECK_FALSE(generator.part(9).drum_keys.level(51).has_value()); // and MAP1 writes no longer land
}

TEST_CASE("a program change on a drum part discards its per-key overrides", "[stream][sccore]")
{
    // Reloading the kit overwrites the per-key planes, so anything the drum-setup NRPNs or SysEx
    // wrote into them is gone. Measured with `scdec gsdrumnrpn`: write pan 100 to key 49, strike
    // it, send a program change, strike again -- the plane reads the kit's own 84. Level, coarse
    // pitch, reverb and chorus behave the same.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    generator.send_sysex(dt1({0x41, 0x02, 40, 100}));
    generator.send_sysex(dt1({0x41, 0x04, 40, 111}));
    REQUIRE(generator.part(9).drum_keys.level(40) == 100);
    REQUIRE(generator.part(9).drum_keys.pan(40) == 111);

    SECTION("a program that names a kit clears them")
    {
        generator.send_channel(0xC9, 8, 0); // Room
        CHECK_FALSE(generator.part(9).drum_keys.level(40).has_value());
        CHECK_FALSE(generator.part(9).drum_keys.pan(40).has_value());
    }

    SECTION("even when it selects the kit already loaded")
    {
        // The module clears on the reload, not on the kit changing: program 0 into a part already
        // on Standard still wipes them.
        generator.send_channel(0xC9, 0, 0);
        CHECK_FALSE(generator.part(9).drum_keys.level(40).has_value());
    }

    SECTION("a program that names no kit leaves them alone")
    {
        // The condition is that the kit resolves. On the SC-55 drum row programs 0, 1 and 8 are
        // Standard 1, Standard 2 and Room and all three clear; 7 and 63 name nothing, and there
        // the overrides survive -- measured both ways round on the module.
        generator.send_channel(0xC9, 7, 0);
        CHECK(generator.part(9).drum_keys.level(40) == 100);
        generator.send_channel(0xC9, 63, 0);
        CHECK(generator.part(9).drum_keys.pan(40) == 111);

        generator.send_channel(0xC9, 1, 0); // Standard 2 -- resolves, so it clears
        CHECK_FALSE(generator.part(9).drum_keys.level(40).has_value());
    }

    SECTION("a melodic part's program change touches nothing")
    {
        generator.send_channel(0xC0, 48, 0);
        CHECK(generator.part(9).drum_keys.level(40) == 100);
    }
}

TEST_CASE("each drum map carries its own kit", "[stream][sccore]")
{
    // The module keeps a kit buffer per (port, map) -- part_assign_tone addresses
    // `port * 2 + map` -- so a MAP2 rhythm part's program change must not clobber the MAP1 kit
    // beside it. transcendental.mid is the real case: channel 10 is a MAP2 rhythm part cycling
    // its own kits while channel 9 holds one on MAP1, and folding both into one slot per port
    // had channel 9 playing channel 10's kit for most of the song.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    REQUIRE(generator.drum_kit_for(0) == 0);

    // Channel 10 becomes a MAP2 rhythm part and selects the Room kit; MAP1 must not move.
    generator.send_sysex(dt1({0x40, 0x1A, 0x15, 0x02}));
    generator.send_channel(0xCA, 8, 0);
    CHECK(generator.drum_kit_for(0) == 0);

    // The default rhythm part still owns the MAP1 slot.
    generator.send_channel(0xC9, 8, 0);
    CHECK(generator.drum_kit_for(0) != 0);
}

TEST_CASE("a drum key's receive switches govern its whole life", "[stream][sccore]")
{
    // No timer bounds a drum voice. The kit record says, per key, whether it answers note-off
    // (bit 0 of the receive plane) and note-on (bit 4), drum-setup SysEx rewrites both live
    // (`drum_setup_rx_noteoff` / `drum_setup_rx_noteon`, parameters 07 and 08), and everything
    // else is the envelope's own business. Key 25 of the Standard kit is the snare roll -- the
    // kit's one Rx Note Off key, a looping wave whose envelope sustains until the file lets go.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    std::vector<float> left(ToneGenerator::block_size);
    std::vector<float> right(ToneGenerator::block_size);
    const auto render_seconds = [&](double seconds, bool stop_when_silent = false) {
        const auto blocks = static_cast<int>(seconds * ToneGenerator::sample_rate
                                             / ToneGenerator::block_size);
        for (int block = 0; block < blocks; ++block) {
            generator.render(left, right);
            if (stop_when_silent && generator.active_voices() == 0) {
                break;
            }
        }
    };

    SECTION("the snare roll sustains while held and releases at note-off")
    {
        generator.send_channel(0x99, 25, 100);
        // Three seconds in -- a second past where the old 1.8 s ring cut it -- still rolling.
        render_seconds(3.0);
        CHECK(generator.active_voices() > 0);

        generator.send_channel(0x89, 25, 64);
        render_seconds(4.0, /*stop_when_silent=*/true);
        CHECK(generator.active_voices() == 0);
    }

    SECTION("held forever, it rolls forever, exactly like the module")
    {
        generator.send_channel(0x99, 25, 100);
        render_seconds(6.0);
        CHECK(generator.active_voices() > 0);
    }

    SECTION("drum setup can revoke Rx Note Off, and the note-off then bounces")
    {
        generator.send_sysex(dt1({0x41, 0x07, 25, 0x00}));
        generator.send_channel(0x99, 25, 100);
        render_seconds(1.0);
        generator.send_channel(0x89, 25, 64);
        render_seconds(3.0);
        CHECK(generator.active_voices() > 0);
    }

    SECTION("drum setup can revoke Rx Note On, and the key falls silent")
    {
        generator.send_sysex(dt1({0x41, 0x08, 38, 0x00}));
        generator.send_channel(0x99, 38, 100);
        CHECK(generator.note_count() == 0);
        CHECK(generator.active_voices() == 0);

        // Switched back on, the key sounds again.
        generator.send_sysex(dt1({0x41, 0x08, 38, 0x01}));
        generator.send_channel(0x99, 38, 100);
        CHECK(generator.note_count() == 1);
        CHECK(generator.active_voices() > 0);
    }
}

TEST_CASE("a drum part's pan follows CC#10, the kit plane offsetting it", "[stream][sccore]")
{
    // The kit's `+0x280` plane is an OFFSET from centre, not the absolute position it reads like.
    // The engine bases a drum voice on the part panpot exactly as it does a melodic one and folds
    // the plane in on top -- `pan = clamp(part[0x3dd] + (kit[0x280 + key] - 0x40))`, the pan setup
    // at 180060620 -- so CC#10 sweeps a drum part across the field.
    //
    // **This has to be its own test, because the song gate cannot see it.** Every metric there is
    // taken on the mono sum, which is by construction blind to where in the image a voice sits:
    // read as absolute, this port pinned SOMDesert-SC8850.mid's hand clap at the plane's own 54
    // for the whole song while the module swept it 9..115, and `MAKORO.MID` and `bad_apple`, both
    // in the corpus and both panning drums, moved by a thousandth of a dB. Measured against the
    // module, 151 of that file's 153 claps now land on its exact pan and all 153 within one step.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    // Key 42, the closed hi-hat, sits well right of centre in the default kit. A centred key could
    // not tell "the panpot replaces the plane" from "the plane offsets the panpot" -- they agree
    // wherever the plane is 64, which is most of a kit and every key of a kick-and-snare probe.
    constexpr int hat = 42;
    const int plane = notes.drums().key(hat).pan;
    REQUIRE(plane != PanLaw::centre);

    std::vector<float> left(ToneGenerator::block_size);
    std::vector<float> right(ToneGenerator::block_size);

    /// One strike, dry, as the right channel's *share* of the hit -- 0 hard left, 1 hard right.
    /// A share rather than a right-over-left ratio because the pan law's ends are silent on one
    /// side, and a ratio there is a division by zero that no tolerance can express.
    ///
    /// The voice is one mono signal times two gains, so the share of its RMS is the share of the
    /// gains exactly, and this compares against `PanLaw::gains` with nothing fitted.
    ///
    /// The sends are silenced first: their wet is a mono bus that arrives at its own position and
    /// would dilute the very reading being taken.
    ///
    /// Struck into silence, and left in silence: a hit measured over the tail of the one before it
    /// reads a blend of two positions, which matters as soon as a caller strikes twice.
    const auto strike = [&](ToneGenerator& generator, int note) {
        generator.send_channel(0xB9, 91, 0);
        generator.send_channel(0xB9, 93, 0);
        generator.send_channel(0xB9, 94, 0);
        generator.send_channel(0x99, note, 100);

        double left_energy = 0.0;
        double right_energy = 0.0;
        for (int block = 0; block < 400; ++block) {
            generator.render(left, right);
            for (std::size_t i = 0; i < left.size(); ++i) {
                left_energy += static_cast<double>(left[i]) * static_cast<double>(left[i]);
                right_energy +=
                    static_cast<double>(right[i]) * static_cast<double>(right[i]);
            }
        }
        REQUIRE(left_energy + right_energy > 0.0);

        for (int block = 0; block < 4000 && generator.active_voices() > 0; ++block) {
            generator.render(left, right);
        }

        const double left_rms = std::sqrt(left_energy);
        const double right_rms = std::sqrt(right_energy);
        return right_rms / (left_rms + right_rms);
    };

    /// The same share, from the pan law, for a voice the engine placed at `position`.
    const auto expected_share = [&](int position) {
        const auto [gain_left, gain_right] = notes.pan().gains(std::clamp(position, 0, 127));
        return gain_right / (gain_left + gain_right);
    };

    SECTION("CC#10 moves the hit, and the plane rides on top of it")
    {
        for (const int panpot : {1, 32, 64, 96, 127}) {
            INFO("CC#10 " << panpot << ", kit plane " << plane);
            ToneGenerator generator{notes};
            generator.send_channel(0xB9, 10, panpot);
            CHECK_THAT(strike(generator, hat),
                       WithinAbs(expected_share(panpot + plane - PanLaw::centre), 1e-4));
        }
    }

    SECTION("at a centred panpot the hit lands on the plane alone")
    {
        // The case that let the bug hide: with nothing sent, the part sits at 64 and the offset is
        // zero, so an absolute reading and this one agree exactly. Pinned here so that agreement
        // stays a consequence rather than the rule.
        ToneGenerator generator{notes};
        CHECK_THAT(strike(generator, hat), WithinAbs(expected_share(plane), 1e-4));
    }

    SECTION("CC#10 zero is pan 1, not the random draw")
    {
        // The wheel cannot reach RND: the engine clamps a zero CC#10 to one, so this is a hard-left
        // hit and the same one every time.
        ToneGenerator generator{notes};
        generator.send_channel(0xB9, 10, 0);
        CHECK_THAT(strike(generator, hat), WithinAbs(expected_share(1 + plane - PanLaw::centre), 1e-4));
    }

    SECTION("a SysEx panpot of zero draws a fresh position per hit")
    {
        // GS RND, and it wins over the plane outright -- the engine draws and returns before it
        // ever folds the offset in. Measured on the module, eight strikes of one key at panpot 0
        // land at 62, 45, 22, 14, 32, 95, 86 and 0, so what is asserted is that the hits differ,
        // not where they fall: this port's PRNG is not stream-aligned with the module's.
        ToneGenerator generator{notes};
        generator.send_sysex(dt1({0x40, 0x10, 0x1C, 0x00}));

        std::vector<double> shares;
        for (int hit = 0; hit < 8; ++hit) {
            shares.push_back(strike(generator, hat));
        }
        const auto [low, high] = std::minmax_element(shares.begin(), shares.end());
        INFO("right-channel share spans " << *low << " to " << *high);
        CHECK(*high - *low > 0.2);
    }
}

TEST_CASE("the NRPN-dropped crash still sounds on the SC-55 map", "[stream][sccore]")
{
    // WATRWLD1.MID drops its crash (key 55) forty steps with the drum pitch NRPN. On the SC-55
    // map the Standard kit's crash tone follows pitch at 100%, so the old doubled, unclamped
    // plane fell five octaves instead of the engine's 2.3 and the crash disappeared into
    // subsonics. The corrected plane -- offset added one-for-one, clamped at zero -- keeps it a
    // deep but audible cymbal.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGeneratorOptions options;
    options.map = ToneMap::sc55;
    ToneGenerator generator{notes, options};

    const auto crossings = [&](bool with_nrpn) {
        generator.reset();
        if (with_nrpn) {
            generator.send_channel(0xB9, 99, 0x18);
            generator.send_channel(0xB9, 98, 55);
            generator.send_channel(0xB9, 6, 24);
        }
        generator.send_channel(0x99, 55, 127);

        std::vector<float> left(ToneGenerator::sample_rate);
        std::vector<float> right(left.size());
        generator.render(left, right);

        int count = 0;
        for (std::size_t i = 1; i < left.size(); ++i) {
            const float previous = left[i - 1] + right[i - 1];
            const float current = left[i] + right[i];
            if ((previous < 0.0F) != (current < 0.0F)) {
                ++count;
            }
        }
        return count;
    };

    const int plain = crossings(false);
    const int dropped = crossings(true);

    // The NRPN lowers the pitch, so the spectrum moves down -- but it must stay in hearing
    // range. Measured: ~9900 crossings a second for the untouched key, ~3900 for the corrected
    // drop, and only a few hundred for the five-octave fall this regression pins. The bounds sit
    // well clear of all three.
    CHECK(plain > 6000);
    CHECK(dropped < plain);
    CHECK(dropped > 2000);
}

TEST_CASE("the four-band EQ engages only when a part opts in", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    // Renders the same note, optionally switching the part's EQ on and cutting the low band hard.
    const auto render = [&](bool part_eq, bool cut_low) {
        ToneGenerator generator{notes};
        if (part_eq) {
            generator.send_sysex(dt1({0x40, 0x41, 0x20, 0x01}));
        }
        if (cut_low) {
            generator.send_sysex(dt1({0x40, 0x02, 0x01, 0x34}));
        }
        CHECK(generator.part(0).eq_enabled == part_eq);

        generator.send_channel(0x90, 40, 100);
        std::vector<float> left(32000);
        std::vector<float> right(32000);
        generator.render(left, right);

        double energy = 0.0;
        for (const float sample : left) {
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }
        return energy;
    };

    const double plain = render(false, false);
    REQUIRE(plain > 0.0);

    // Switching the EQ on with nothing set changes nothing: flat is exactly transparent, and a
    // part on the EQ bus has to sound the same as one that is not.
    CHECK(render(true, false) == plain);

    // Cutting the low band while no part is on the EQ bus also changes nothing, which is the half
    // that catches an EQ wired into the main mix by accident.
    CHECK(render(false, true) == plain);

    // Both together, and a low note loses energy.
    CHECK(render(true, true) < plain * 0.95);
}

TEST_CASE("the control matrix arrives over SysEx", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    using Source = ControlMatrix::Source;
    using Destination = ControlMatrix::Destination;

    // Power-on: everything centred or zero, with the one exception that gives a GM file's mod
    // wheel its vibrato without being asked.
    const Part& part = generator.part(0);
    CHECK(part.control.at(Source::modulation, Destination::pitch) == 0x40);
    CHECK(part.control.at(Source::modulation, Destination::lfo1_pitch) == 0x0A);
    CHECK(part.control.at(Source::modulation, Destination::lfo1_tva) == 0x00);
    CHECK(part.control.at(Source::cc2, Destination::lfo2_rate) == 0x40);

    // The address is a source in the high nibble and a destination in the low one. Block 1 is
    // channel 0; `40 21 26` is CC1 -> LFO1 TVA depth.
    generator.send_sysex(dt1({0x40, 0x21, 0x46, 0x50}));
    CHECK(part.control.at(Source::cc1, Destination::lfo1_tva) == 0x50);

    generator.send_sysex(dt1({0x40, 0x21, 0x04, 0x7F}));
    CHECK(part.control.at(Source::modulation, Destination::lfo1_pitch) == 0x7F);

    // Bend's pitch depth is not a matrix cell here, because it is not one in the engine: it is the
    // same byte RPN 00/00 writes, so `40 2x 10` sets the bend range and nothing else.
    CHECK(part.bend_range == 2);
    generator.send_sysex(dt1({0x40, 0x21, 0x10, 0x4C}));
    CHECK(part.bend_range == 12);

    // And the reverse: the RPN sets what the SysEx would have read back.
    generator.send_channel(0xB0, 101, 0);
    generator.send_channel(0xB0, 100, 0);
    generator.send_channel(0xB0, 6, 7);
    CHECK(part.bend_range == 7);

    // Both clamp at 24 semitones, which is where the engine clamps.
    generator.send_sysex(dt1({0x40, 0x21, 0x10, 0x7F}));
    CHECK(part.bend_range == 24);

    // Addresses past the matrix are ignored rather than written past the end.
    generator.send_sysex(dt1({0x40, 0x21, 0x0B, 0x7F}));
    generator.send_sysex(dt1({0x40, 0x21, 0x60, 0x7F}));
    CHECK(part.control.at(Source::cc2, Destination::lfo2_tva) == 0x00);
}

TEST_CASE("polyphonic aftertouch bends the key it names", "[stream][sccore]")
{
    // Verified against the DLL rather than reasoned about. A part with `40 2x 30` at 0x58, one note
    // held while its pressure ramps 0 to 127: the module takes the note from 130 Hz to 1032 Hz and
    // this engine follows it to the same four frequencies, measured by autocorrelation at 0.35,
    // 0.70, 0.95 and 1.20 s.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // Poly pressure -> pitch on part 1, deflected hard enough to be unmistakable.
    generator.send_sysex(dt1({0x40, 0x21, 0x30, 0x58}));
    const Part& part = generator.part(0);

    generator.send_channel(0xC0, 48, 0);
    generator.send_channel(0x90, 60, 100);
    generator.send_channel(0x90, 67, 100);

    const double unpressed = part.matrix_pitch_milli_semitones(part.key_pressure(60));
    CHECK(unpressed == 0.0);

    // The pressure lands on the key it names and on no other, which is the whole of "polyphonic".
    generator.send_channel(0xA0, 60, 127);
    CHECK(part.key_pressure(60) == 127);
    CHECK(part.key_pressure(67) == 0);

    const double pressed = part.matrix_pitch_milli_semitones(part.key_pressure(60));
    CHECK(pressed > 0.0);
    CHECK(part.matrix_pitch_milli_semitones(part.key_pressure(67)) == 0.0);

    // Half the pressure, less than half the way there is not asserted -- only that it tracks.
    generator.send_channel(0xA0, 60, 64);
    CHECK(part.matrix_pitch_milli_semitones(part.key_pressure(60)) < pressed);

    // **The measured surprise.** A fresh strike does not clear it: press a key hard, release it,
    // strike it again saying nothing about pressure, and the module sounds the new note still bent.
    // The pressure belongs to the key, not to the note that was sounding on it.
    generator.send_channel(0xA0, 60, 127);
    generator.send_channel(0x80, 60, 0);
    generator.send_channel(0x90, 60, 100);
    CHECK(part.key_pressure(60) == 127);
    CHECK(part.matrix_pitch_milli_semitones(part.key_pressure(60)) == pressed);

    // A GS reset is what does clear it.
    generator.reset();
    CHECK(generator.part(0).key_pressure(60) == 0);
}

TEST_CASE("the mod wheel's depth comes from the matrix", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto depth_at = [&](int assigned) {
        ToneGenerator generator{notes};
        if (assigned >= 0) {
            generator.send_sysex(dt1({0x40, 0x21, 0x04, assigned}));
        }
        generator.send_channel(0xB0, 1, 127);
        return static_cast<double>(generator.part(0).matrix().lfo1_pitch);
    };

    // Untouched, the matrix's 0x0a is what the engine assumed all along, so a stream that never
    // addresses `40 2x 04` is unchanged.
    const double power_on = depth_at(-1);
    CHECK(depth_at(0x0A) == power_on);
    CHECK(power_on > 0.0);

    // Assigning more deepens it and assigning nothing silences it -- neither of which happened
    // before the matrix was wired in.
    CHECK(depth_at(0x40) > power_on * 3.0);
    CHECK(depth_at(0x00) == 0.0);
}

TEST_CASE("the control matrix reaches amplitude, not only pitch", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    // A destination is only implemented once a render moves. The matrix's own arithmetic is tested
    // in the modulation suite; what this asks is whether the sum reaches the mix at all.
    const auto loudness = [&](int depth, int wheel) {
        ToneGenerator generator{notes};
        // The wheel is never inert at power-on: `40 2x 04` starts at 0x0a, so moving it adds
        // vibrato whatever else it is assigned to, and that alone shifts the measured energy by a
        // few per cent. Switching that route off is what leaves the amplitude destination as the
        // only thing the wheel is doing.
        generator.send_sysex(dt1({0x40, 0x21, 0x04, 0x00}));
        generator.send_sysex(dt1({0x40, 0x21, 0x02, depth}));
        generator.send_channel(0xB0, 1, wheel);
        generator.send_channel(0x90, 60, 100);

        std::array<float, 32000> left{};
        std::array<float, 32000> right{};
        generator.render(left, right);

        double energy = 0.0;
        for (std::size_t i = 0; i < left.size(); ++i) {
            energy += static_cast<double>(left[i]) * static_cast<double>(left[i])
                      + static_cast<double>(right[i]) * static_cast<double>(right[i]);
        }
        return std::sqrt(energy / left.size());
    };

    // Centred, the wheel does nothing whatever it is set to.
    const double neutral = loudness(0x40, 0);
    REQUIRE(neutral > 0.0);
    CHECK_THAT(loudness(0x40, 127), WithinRel(neutral, 1e-9));

    // Assigned, it does -- upward at a positive depth, downward at a negative one, and the module
    // was measured at very close to a doubling with the depth and the wheel both at maximum.
    const double lifted = loudness(0x7F, 127);
    const double cut = loudness(0x00, 127);
    CHECK(lifted > neutral * 1.5);
    CHECK(cut < neutral * 0.75);

    // Half the wheel is not half the effect in decibels, but it is between the two ends, which is
    // enough to show the amount is carried through rather than treated as a switch.
    const double halfway = loudness(0x7F, 64);
    CHECK(halfway > neutral);
    CHECK(halfway < lifted);
}

TEST_CASE("the assignable controllers reach the matrix", "[stream][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    // "CC1" here is the GS assignable source, not Control Change #1. The mod wheel is a different
    // source with a fixed number; these two listen to whatever `40 1x 1F` and `20` name, and start
    // pointed at General Purpose 1 and 2 -- controllers nothing else in this engine reads.
    SECTION("the numbers default to General Purpose 1 and 2")
    {
        ToneGenerator generator{notes};
        CHECK(generator.part(0).cc1_number == 16);
        CHECK(generator.part(0).cc2_number == 17);

        generator.send_channel(0xB0, 16, 100);
        generator.send_channel(0xB0, 17, 40);
        CHECK(generator.part(0).cc1 == 100);
        CHECK(generator.part(0).cc2 == 40);
    }

    SECTION("a reassigned number moves which controller feeds the source")
    {
        ToneGenerator generator{notes};
        generator.send_sysex(dt1({0x40, 0x11, 0x1F, 20}));
        CHECK(generator.part(0).cc1_number == 20);

        generator.send_channel(0xB0, 16, 90);
        CHECK(generator.part(0).cc1 == 0);
        generator.send_channel(0xB0, 20, 90);
        CHECK(generator.part(0).cc1 == 90);
    }

    SECTION("the number is clamped to 95, which the module was asked about")
    {
        // Assigning 100 and then sending CC#95 modulates on the module, while sending CC#16 does
        // not -- so it clamps rather than rejecting the assignment and leaving the default. That
        // distinction is invisible from the assignment alone and was measured, not assumed.
        ToneGenerator generator{notes};
        generator.send_sysex(dt1({0x40, 0x11, 0x1F, 100}));
        CHECK(generator.part(0).cc1_number == 95);

        generator.send_channel(0xB0, 16, 70);
        CHECK(generator.part(0).cc1 == 0);
        generator.send_channel(0xB0, 95, 70);
        CHECK(generator.part(0).cc1 == 70);
    }

    SECTION("pointing a source at a controller does not take its other meaning away")
    {
        // The number is a pointer to a message, not a claim on it. A part whose CC1 is pointed at
        // the mod wheel has a wheel that drives both its own routes and CC1's.
        ToneGenerator generator{notes};
        generator.send_sysex(dt1({0x40, 0x11, 0x1F, 1}));
        generator.send_channel(0xB0, 1, 64);
        CHECK(generator.part(0).cc1 == 64);
        CHECK(generator.part(0).modulation == 64);
    }

    SECTION("the source reaches a destination")
    {
        // `40 21 42` is source 4 (CC1) on destination 2 (amplitude), which the module was measured
        // at +6.0 dB with the depth and the controller both at maximum.
        const auto loudness = [&](int depth, int amount) {
            ToneGenerator generator{notes};
            generator.send_sysex(dt1({0x40, 0x21, 0x42, depth}));
            generator.send_channel(0xB0, 16, amount);
            generator.send_channel(0x90, 60, 100);

            std::array<float, 32000> left{};
            std::array<float, 32000> right{};
            generator.render(left, right);
            double energy = 0.0;
            for (std::size_t i = 0; i < left.size(); ++i) {
                energy += static_cast<double>(left[i]) * static_cast<double>(left[i])
                          + static_cast<double>(right[i]) * static_cast<double>(right[i]);
            }
            return std::sqrt(energy / left.size());
        };

        const double neutral = loudness(0x40, 127);
        REQUIRE(neutral > 0.0);
        CHECK(loudness(0x7F, 127) > neutral * 1.5);
        CHECK(loudness(0x00, 127) < neutral * 0.75);
    }
}

TEST_CASE("a pan change sweeps rather than snapping", "[stream][sccore]")
{
    // `voice_expr_smooth` @`180083db0` slews the pan *position* by at most two of 127 a control
    // tick, so CC#10 moved under a sounding note takes hundreds of milliseconds to arrive. Measured
    // with `scdec panramp`: CC#10 64 -> 127 is 32 steps of two, one every 320 samples, 310 ms end
    // to end. Rendering the same jump through the oracle agrees -- the two balance trajectories
    // track within 0.03 the whole way across.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    generator.send_channel(0xB0, 7, 127);
    // Dry: the sends are not panned, and their tail is enough to keep the balance off its rails.
    generator.send_channel(0xB0, 91, 0);
    generator.send_channel(0xB0, 93, 0);
    generator.send_channel(0xB0, 10, PanLaw::centre);
    generator.send_channel(0x90, 96, 110);

    constexpr int window = 1024; // 32 ms
    std::vector<float> left(window);
    std::vector<float> right(window);

    // Right's share of the energy: 0.5 centred, 1.0 hard right. Reading the balance rather than a
    // level keeps the note's own envelope out of it.
    const auto balance = [&] {
        generator.render(left, right);
        double el = 0.0;
        double er = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(window); ++i) {
            el += static_cast<double>(left[i]) * static_cast<double>(left[i]);
            er += static_cast<double>(right[i]) * static_cast<double>(right[i]);
        }
        return el + er > 1e-14 ? er / (el + er) : 0.5;
    };

    for (int i = 0; i < 6; ++i) {
        (void)balance(); // settle the attack
    }
    CHECK_THAT(balance(), WithinAbs(0.5, 0.05));

    generator.send_channel(0xB0, 10, 127);

    std::vector<double> sweep;
    for (int i = 0; i < 16; ++i) {
        sweep.push_back(balance());
    }

    // The first window is barely moved. This is the assertion an unpanned-instantly engine fails:
    // without the slew it would already read 1.0 here.
    CHECK(sweep.front() < 0.7);

    // Monotone the whole way -- the position steps toward the target and never past it.
    for (std::size_t i = 1; i < sweep.size(); ++i) {
        CHECK(sweep[i] >= sweep[i - 1] - 1e-9);
    }

    // Arrival somewhere between 250 and 450 ms, against the oracle's 310. The window is 32 ms wide
    // and lags by about half of one, so this is not tighter than the measurement supports.
    const auto arrived = std::find_if(sweep.begin(), sweep.end(), [](double b) { return b > 0.999; });
    REQUIRE(arrived != sweep.end());
    const auto milliseconds = (arrived - sweep.begin() + 1) * window / 32;
    CHECK(milliseconds >= 250);
    CHECK(milliseconds <= 450);
}

TEST_CASE("a send level slews rather than switching", "[stream][sccore]")
{
    // `voice_send_slew` @`180083be0` steps the send's gain word by 8 of 1024 a control tick, and
    // full scale is 1016/1024 -- so crossing the whole range takes 127 ticks, **1.27 s**, the
    // slowest smoother in the engine by a wide margin. Measured on the reverb send with
    // `scdec sendramp`: CC#91 0 -> 127 is 127 steps of 8/1024, one every 320 samples, 1260 ms.
    //
    // The wet is isolated by differencing against a generator with the reverb switched off. Nothing
    // else differs between the two, so what is left is exactly the reverb's contribution -- the dry
    // path cancels to the last bit, which is what makes this measurable at all.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ToneGeneratorOptions dry_options;
    dry_options.reverb = false;
    ToneGenerator wet{notes};
    ToneGenerator dry{notes, dry_options};

    const auto setup = [](ToneGenerator& g) {
        g.send_channel(0xB0, 7, 127);
        g.send_channel(0xB0, 91, 0);
        g.send_channel(0xB0, 93, 0);
        // A *sustaining* voice, and that matters more than it looks. This held the default piano
        // for a long time, whose level falls twenty decibels across the two seconds measured here
        // -- so the wet return was the send's slew multiplied by the note's decay, and the shape
        // the assertions below read was half decay. `Church Org.` holds its level, which leaves the
        // slew as the only thing moving.
        g.send_channel(0xC0, 19, 0);
        g.send_channel(0x90, 72, 110);
    };
    setup(wet);
    setup(dry);

    constexpr int window = 1600; // 50 ms
    std::vector<float> wl(window);
    std::vector<float> wr(window);
    std::vector<float> dl(window);
    std::vector<float> dr(window);

    const auto wet_rms = [&] {
        wet.render(wl, wr);
        dry.render(dl, dr);
        double sum = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(window); ++i) {
            const double d = static_cast<double>(wl[i]) - static_cast<double>(dl[i]);
            sum += d * d;
        }
        return std::sqrt(sum / window);
    };

    // Send at zero: the two renders are identical, so there is nothing to hear.
    for (int i = 0; i < 4; ++i) {
        CHECK(wet_rms() == 0.0);
    }

    wet.send_channel(0xB0, 91, 127);
    dry.send_channel(0xB0, 91, 127);

    std::vector<double> growth; // 40 windows of 50 ms: 2 s, comfortably past the 1.27 s crossing
    for (int i = 0; i < 40; ++i) {
        growth.push_back(wet_rms());
    }

    // The reverb's own build-up is in here too, so this does not pin the rate on its own -- what it
    // pins is the shape. A send applied instantly puts the bus at full level within one block and
    // the wet would be at its settled value inside the first window or two.
    const double settled = *std::max_element(growth.begin(), growth.end());
    REQUIRE(settled > 0.0);
    CHECK(growth[0] < settled * 0.35);
    CHECK(growth[1] < settled * 0.6);

    // Still visibly climbing at half a second, which is what separates the measured 1.27 s from the
    // 400 ms an earlier reading of this had: a 400 ms slew is finished well before here.
    CHECK(growth[10] < settled * 0.9);

    // And it does arrive, inside the second second.
    CHECK(*std::max_element(growth.begin() + 30, growth.end()) > settled * 0.9);
}

TEST_CASE("CC#74 moves the cutoff once, not twice", "[stream][sccore]")
{
    // The part's cutoff offset is applied by the live per-block path. It used to *also* be latched
    // into the envelope's base at note-on, so a controller step moved the cutoff 0x200 of the
    // 15-bit sum where the module moves 0x100. Nothing caught it, because both halves were
    // individually reasonable and the doubling only shows once CC#74 leaves the neutral 0x40.
    //
    // Measured against the engine's own cutoff word at voice+0xcc, for this note and tone: it walks
    // 244540 -> 205888 between CC#74 64 and 35. Ours walked twice that far, which cost eleven
    // decibels of level on a part that asks for 35 -- one channel of the Cave Story XG set does.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto energy_at = [&](int cc74) {
        ToneGenerator generator{notes};
        generator.send_channel(0xB0, 7, 127);
        generator.send_channel(0xB0, 91, 0);
        generator.send_channel(0xB0, 93, 0);
        generator.send_channel(0xB0, 74, cc74);
        generator.send_channel(0xC0, 38, 0);
        generator.send_channel(0x90, 48, 127);

        std::vector<float> left(32000);
        std::vector<float> right(32000);
        generator.render(left, right);

        double sum = 0.0;
        for (const float sample : left) {
            sum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        return std::sqrt(sum / static_cast<double>(left.size()));
    };

    const double neutral = energy_at(0x40);
    REQUIRE(neutral > 0.0);

    // Closing the filter to 35 costs the module about a decibel on this tone. Doubling the step
    // costs more than ten, so the two are nowhere near each other: anything above half the neutral
    // energy is the single application, anything near a fifth is the double.
    CHECK(energy_at(35) / neutral > 0.5);

    // And the control still does something -- a test that only guarded the doubling would pass on
    // an engine that ignored CC#74 entirely.
    CHECK(energy_at(0) / neutral < 0.9);
}

TEST_CASE("CC#71 opens the filter's resonance, and the wrong way round", "[stream][sccore]")
{
    // This control was recorded as unimplemented in the module for a long time, on the strength of
    // a search that found `part+0x3e7`'s four writers and no readers. The readers were there: Ghidra
    // prints that offset in decimal, so `999` is what the reads spell and a hex search cannot see
    // them. Measured against the DLL, the control is worth 9.2 dB of level across its range.
    //
    // The direction is the trap. `resonance_byte` subtracts the controller, and the byte is
    // reciprocal-Q, so *raising* CC#71 lowers the byte and makes the filter more resonant -- which
    // on this tone means louder. An implementation that got the sign backwards would still pass a
    // test that only asked whether the control did something.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto energy_at = [&](int cc71) {
        ToneGenerator generator{notes};
        generator.send_channel(0xB0, 7, 127);
        generator.send_channel(0xB0, 91, 0);
        generator.send_channel(0xB0, 93, 0);
        generator.send_channel(0xB0, 74, 0x40);
        generator.send_channel(0xB0, 71, cc71);
        generator.send_channel(0xC0, 38, 0);
        generator.send_channel(0x90, 48, 127);

        std::vector<float> left(64000);
        std::vector<float> right(64000);
        generator.render(left, right);

        double sum = 0.0;
        for (const float sample : left) {
            sum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        return std::sqrt(sum / static_cast<double>(left.size()));
    };

    const double neutral = energy_at(0x40);
    REQUIRE(neutral > 0.0);

    // The DLL's own figures for this note and tone, 2 s at 32 kHz: 0.01416 / 0.02504 / 0.04088 at
    // CC#71 0 / 64 / 127. Ratios rather than absolutes, so this does not also pin the master gain.
    CHECK_THAT(energy_at(0) / neutral, WithinAbs(0.5655, 0.05));
    CHECK_THAT(energy_at(127) / neutral, WithinAbs(1.6326, 0.10));

    // The control saturates: the resonance byte floors at 4, which this tone's partials reach by
    // CC#71 96, so everything above that is the same render.
    CHECK_THAT(energy_at(127), WithinRel(energy_at(96), 1e-09));
}

TEST_CASE("CC#71 reaches a note that is already sounding", "[stream][sccore]")
{
    // Stage A re-derives the resonance byte every control tick from the part's live byte, so a
    // resonance sweep bends notes that are already down. Latching it at note-on instead would leave
    // a held chord behind the sweep -- the same mistake the cutoff offset used to make, and the one
    // that makes a filter sweep sound like it starts on the next note instead of now.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto energy_of = [](std::span<const float> block) {
        double sum = 0.0;
        for (const float sample : block) {
            sum += static_cast<double>(sample) * static_cast<double>(sample);
        }
        return std::sqrt(sum / static_cast<double>(block.size()));
    };

    // Two runs, identical up to the half-second mark, differing only in whether the controller
    // arrives after the note is already down. Comparing the second half against the *first* half
    // instead would measure the tone's own decay, which on this tone swamps the control: it loses
    // more over that half second than the filter opening gains back.
    const auto second_half = [&](bool sweep) {
        ToneGenerator generator{notes};
        generator.send_channel(0xB0, 7, 127);
        generator.send_channel(0xB0, 91, 0);
        generator.send_channel(0xB0, 93, 0);
        generator.send_channel(0xC0, 38, 0);
        generator.send_channel(0x90, 48, 127);

        std::vector<float> left(16000);
        std::vector<float> right(16000);
        generator.render(left, right);
        const double first = energy_of(left);

        // No new note -- only the controller, and only in one of the two runs.
        if (sweep) {
            generator.send_channel(0xB0, 71, 127);
        }
        generator.render(left, right);
        return std::pair{first, energy_of(left)};
    };

    const auto [held_first, held] = second_half(false);
    const auto [swept_first, swept] = second_half(true);

    // The runs really were identical up to the message.
    REQUIRE(held_first > 0.0);
    CHECK_THAT(swept_first, WithinRel(held_first, 1e-09));

    // And the controller moved the note that was already sounding. Latching at note-on would leave
    // these two *bit-identical*, so strictly the margin only has to exclude numerical noise; the
    // measured figure is 1.21, and the bound is set below that rather than at it so the test does
    // not re-break on any change that moves the filter slightly without breaking the routing.
    CHECK(swept > held * 1.1);
}

TEST_CASE("the GS tone map SysEx selects a part's map", "[stream][sccore]")
{
    // `40 4x 01` is the tone map and `40 4x 00` is not. Measured with a sweep of every address in
    // the `40 1x` and `40 4x` blocks against a part dump: exactly two of the 256 move either byte,
    // `00` writing `part+0x44d` unclamped and `01` writing `part+0x44e` clamped to 1-4. The second
    // is the map, and it is how every tier 2 fixture here picks one -- the `scdec` harness sends it
    // to all sixteen blocks after a GS reset.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // `40 4x pp vv`, with the block in `x`: channel 10 is block 0, channels 1-9 are blocks 1-9.
    const auto tone_map_sysex = [](int block, int parameter, int value) {
        std::array<std::uint8_t, 4> payload{
            0x40, static_cast<std::uint8_t>(0x40 | block),
            static_cast<std::uint8_t>(parameter), static_cast<std::uint8_t>(value)};
        int sum = 0;
        for (const std::uint8_t byte : payload) {
            sum += byte;
        }
        return std::vector<std::uint8_t>{0xF0, 0x41, 0x10, 0x42, 0x12, payload[0], payload[1],
                                         payload[2], payload[3],
                                         static_cast<std::uint8_t>((128 - (sum & 0x7F)) & 0x7F),
                                         0xF7};
    };

    SECTION("`01` sets it, per part")
    {
        // The address carries a **block**; this engine's part array is indexed by channel, so the
        // two are converted at the boundary. Block 1 is channel 1, which is index 0; block 0 is
        // channel 10, index 9. Getting that backwards is the classic way to read a part nobody
        // wrote and conclude the message does nothing.
        generator.send_sysex(tone_map_sysex(1, 0x01, 1));
        generator.send_sysex(tone_map_sysex(2, 0x01, 3));
        generator.send_sysex(tone_map_sysex(0, 0x01, 2));
        CHECK(generator.part_tone_map(0) == ToneMap::sc55);
        CHECK(generator.part_tone_map(1) == ToneMap::sc88pro);
        CHECK(generator.part_tone_map(9) == ToneMap::sc88);
        // Untouched parts keep the default rather than following their neighbours.
        CHECK(generator.part_tone_map(3) == ToneGeneratorOptions{}.map);
    }

    SECTION("out of range is dropped, not stored")
    {
        // The module tests `(byte)(value - 1) < 4` and writes nothing otherwise, so a zero leaves
        // the map where it was instead of returning it to the default.
        generator.send_sysex(tone_map_sysex(1, 0x01, 2));
        REQUIRE(generator.part_tone_map(0) == ToneMap::sc88);
        generator.send_sysex(tone_map_sysex(1, 0x01, 0));
        CHECK(generator.part_tone_map(0) == ToneMap::sc88);
        generator.send_sysex(tone_map_sysex(1, 0x01, 5));
        CHECK(generator.part_tone_map(0) == ToneMap::sc88);
    }

    SECTION("the vintage is a default, not a ceiling")
    {
        // The SysEx and CC#32 write the same byte and neither limits the other, so the last one
        // wins. Measured both ways round on the module: SysEx map 1 then CC#32 = 4 renders exactly
        // as a native map 4, and CC#32 = 4 then SysEx map 1 renders exactly as a native map 1.
        generator.send_sysex(tone_map_sysex(1, 0x01, 1));
        generator.send_channel(0xB0, 32, 4);
        CHECK(generator.part_tone_map(0) == ToneMap::sc8820);

        generator.send_sysex(tone_map_sysex(1, 0x01, 1));
        CHECK(generator.part_tone_map(0) == ToneMap::sc55);
    }

    SECTION("`00` is a different parameter and does not touch the map")
    {
        // Worth pinning because a real player gets this wrong. Cog's `gs_bank_lsb_sysex` builds
        // this exact message with `00` as its third address byte and the map as its data, and
        // rendered through the module it changes nothing at all -- verified against `01`, which
        // moves the same file's render.
        generator.send_sysex(tone_map_sysex(1, 0x00, 1));
        CHECK(generator.part_tone_map(0) == ToneGeneratorOptions{}.map);
    }
}

TEST_CASE("the event pipeline holds a message for whole chunks", "[stream][sccore]")
{
    // The module never applies a MIDI message on arrival: `TG_ShortMidiIn` puts it in a ring,
    // `TG_Process` walks it into a staging ring, raises a task bit, and dispatches that bit on a
    // later chunk. Four stages, and the note-on latency measured against the oracle is 128 samples
    // -- four of the engine's 32-sample chunks.
    //
    // The unit is the chunk rather than the host call because `TG_Process` over-renders: it serves
    // whatever the previous call left buffered before rendering anything new, so a host asking for
    // a length that is not a whole number of chunks leaves a remainder and the grid drifts against
    // the host's boundaries.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto onset_with = [&](int delay) {
        ToneGeneratorOptions options;
        options.event_delay_blocks = delay;
        ToneGenerator generator{notes, options};
        generator.send_channel(0xB0, 7, 127);
        generator.send_channel(0xB0, 91, 0);
        generator.send_channel(0xB0, 93, 0);
        generator.send_channel(0xC0, 99, 0);
        generator.send_channel(0x90, 48, 127);

        std::vector<float> left(8000);
        std::vector<float> right(8000);
        generator.render(left, right);

        for (std::size_t i = 0; i < left.size(); ++i) {
            if (std::abs(left[i]) > 1e-4F) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    // Off is the default and sounds at once, which is what every other test here relies on.
    REQUIRE(onset_with(0) == 1);

    // On, each chunk is a whole 32 samples and nothing in between.
    const int one = onset_with(1);
    const int four = onset_with(4);
    CHECK(one == 33);
    CHECK(four == 129);
    CHECK(four - one == 3 * ToneGenerator::block_size);

    // Four chunks is the module's own 128, which is the number this exists to reproduce: the
    // oracle's peak for this note lands 148 samples after ours with the pipeline off, and 20 after
    // it with the pipeline on. The remaining 20 is not staging -- `tg_output_filter` runs even when
    // the host rate already matches the engine's, with a live allpass coefficient of 1/3 over six
    // state slots, and its delay has not been separated out yet.
    CHECK(four - onset_with(0) == 128);
}

TEST_CASE("a message is placed inside the buffer by its sample offset", "[stream][sccore]")
{
    // `TG_ShortMidiIn` does not keep the offset it is handed. It converts it to milliseconds --
    // `offset * 1000 / g_host_sample_rate` -- and stamps the message with that; `TG_Process` then
    // releases the message once its chunk counter reaches the stamp. The comparison works because
    // a 32-sample chunk at the engine's 32 kHz is exactly one millisecond.
    //
    // So placement inside a buffer is quantised to the millisecond, and that quantisation is the
    // module's rather than an approximation: an offset of 1000 samples is 31 ms, not 31.25, and
    // lands at 992.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    const auto onset_at = [&](int offset) {
        ToneGenerator generator{notes};
        generator.send_channel(0xB0, 7, 127);
        generator.send_channel(0xB0, 91, 0);
        generator.send_channel(0xB0, 93, 0);
        generator.send_channel(0xC0, 99, 0);
        generator.send_channel_at(offset, 0x90, 48, 127);

        std::vector<float> left(16000);
        std::vector<float> right(16000);
        generator.render(left, right);

        for (std::size_t i = 0; i < left.size(); ++i) {
            if (std::abs(left[i]) > 1e-4F) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    const int base = onset_at(ToneGenerator::immediately);
    REQUIRE(base == 1);

    // One chunk per millisecond of the offset, and the truncation is the module's own.
    CHECK(onset_at(32) - base == 32);
    CHECK(onset_at(100) - base == 96);
    CHECK(onset_at(320) - base == 320);
    CHECK(onset_at(1000) - base == 992);
    CHECK(onset_at(3200) - base == 3200);
}
