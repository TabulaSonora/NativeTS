#include "tabulasonora/sequence.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <vector>

using namespace ts;
namespace fs = std::filesystem;

namespace {

/// One file and the fixture recording what an independent reader made of it.
struct Case {
    const char* midi;
    const char* fixture;
    const char* shape;
};

/// Both SMF shapes. The format-1 file is the one that matters most here: only a multi-track file
/// exercises the merge -- the (tick, order) sort and the tempo map walked across interleaved
/// tracks -- and a single-track format 0 cannot reach that code at all.
constexpr std::array<Case, 2> all_cases{{
    {"canyon.mid", "canyon_sequence.json", "format 0, 1 track"},
    {"canyon-format1.mid", "canyon_format1_sequence.json", "format 1, 8 tracks"},
}};

} // namespace

TEST_CASE("a MIDI file parses to the same events and notes as an independent reader",
          "[midi][gate]")
{
    // The MIDI half of Phase 5's gate: every event and every note of a real file, against a reader
    // written from the SMF specification rather than translated from this code.
    //
    // What it is really guarding is the sort. Same-position events must keep their tick order, so
    // the final sort has to be stable -- an unstable one can put a note-on ahead of the program
    // change that selects its patch, which does not crash and does not look wrong in a diff of
    // positions. It sounds like the wrong instrument.
    std::size_t files = 0;
    for (const Case& test_case : all_cases) {
        const fs::path midi = testdata::repository_root() / "testdata" / test_case.midi;
        const fs::path fixture_path = testdata::repository_root() / "fixtures" / test_case.fixture;
        if (!fs::exists(midi) || !fs::exists(fixture_path)) {
            continue;
        }
        INFO(test_case.midi << " (" << test_case.shape << ")");
        ++files;

        std::ifstream stream{fixture_path};
        REQUIRE(stream);
        const nlohmann::json document = nlohmann::json::parse(stream);

        const std::vector<MidiEvent> events = smf::read(midi);
        const auto& expected_events = document.at("events");

        REQUIRE(events.size() == expected_events.size());
        REQUIRE(events.size() > 1000);

        for (std::size_t i = 0; i < events.size(); ++i) {
            INFO("event " << i);
            const auto& expected = expected_events[i];
            REQUIRE(events[i].position == expected.at("position").get<std::int64_t>());

            if (expected.at("kind").get<std::string>() == "sysex") {
                REQUIRE(events[i].kind == MidiEventKind::sysex);
                const auto& bytes = expected.at("sysex");
                REQUIRE(events[i].sysex.size() == bytes.size());
                for (std::size_t b = 0; b < bytes.size(); ++b) {
                    REQUIRE(events[i].sysex[b] == bytes[b].get<int>());
                }
            } else {
                REQUIRE(events[i].kind == MidiEventKind::channel);
                REQUIRE(events[i].status == expected.at("status").get<int>());
                REQUIRE(events[i].data1 == expected.at("data1").get<int>());
                REQUIRE(events[i].data2 == expected.at("data2").get<int>());
            }
        }

        const Sequence sequence = sequence_builder::build(events);
        const auto& expected_notes = document.at("notes");

        REQUIRE(sequence.notes.size() == expected_notes.size());
        REQUIRE(sequence.notes.size() > 1000);
        CHECK(sequence.last_event_position == document.at("lastEventPosition").get<std::int64_t>());

        for (std::size_t i = 0; i < sequence.notes.size(); ++i) {
            const NoteRecord& note = sequence.notes[i];
            const auto& expected = expected_notes[i];
            INFO("note " << i << " channel " << note.channel << " note " << note.note);

            REQUIRE(note.channel == expected.at("channel").get<int>());
            REQUIRE(note.note == expected.at("note").get<int>());
            REQUIRE(note.velocity == expected.at("velocity").get<int>());
            REQUIRE(note.on == expected.at("on").get<std::int64_t>());
            REQUIRE(note.off == expected.at("off").get<std::int64_t>());
            REQUIRE(note.program == expected.at("program").get<int>());
            REQUIRE(note.bank == expected.at("bank").get<int>());
            REQUIRE(note.pan == expected.at("pan").get<int>());
            REQUIRE(note.volume == expected.at("volume").get<int>());
            REQUIRE(note.expression == expected.at("expression").get<int>());
            REQUIRE(note.reverb_send == expected.at("reverbSend").get<int>());
            REQUIRE(note.chorus_send == expected.at("chorusSend").get<int>());
            REQUIRE(note.delay_send == expected.at("delaySend").get<int>());
            REQUIRE(note.drum_pitch == expected.at("drumPitch").get<int>());
        }
    }

    if (files == 0) {
        SKIP("No sequence fixtures. Generate them with:\n"
             "  python3 tools/dump_sequence.py testdata/canyon.mid fixtures/canyon_sequence.json");
    }
    // Both shapes must actually be present, or the multi-track merge is untested.
    CHECK(files == all_cases.size());
}

TEST_CASE("event positions land on the 32-sample render grid", "[midi]")
{
    // Needs no file. Every position floors onto the block grid, which is the granularity the engine
    // actually applies events at.
    // Rounding happens first, then the floor onto the grid -- not the other way round.
    CHECK(smf::quantise(0.0) == 0);
    CHECK(smf::quantise(31.0) == 0);
    CHECK(smf::quantise(32.0) == 32);
    CHECK(smf::quantise(63.5) == 64);
    CHECK(smf::quantise(63.9) == 64);
    CHECK(smf::quantise(64.5) == 64);
    CHECK(smf::quantise(1'000'000.0) == 1'000'000);
    CHECK(smf::quantise(1'000'001.0) == 1'000'000);
}

TEST_CASE("the rounding mode does not reach the block grid", "[midi]")
{
    // Worth pinning because it is a claim the port started out believing the other way round.
    //
    // .NET's Math.Round is banker's rounding and C's round() is half-away-from-zero, so the two
    // disagree at every exact tie. They never disagree about the *block*: the candidates differ by
    // one, and one cannot straddle a multiple of 32 at a tie -- a tie sits at n + 0.5, the modes
    // differ only when n is even, and n + 1 is then odd.
    //
    // Swept over 400,000 half-sample positions: 100,000 ties, zero block differences. So this
    // expression is safe either way, and nearbyint is used to match the original rather than
    // because it is load-bearing. Where no grid follows a Math.Round -- the delay's tap lengths --
    // that reasoning does not apply.
    const auto half_away = [](double x) {
        return static_cast<std::int64_t>(std::floor(x + 0.5)) / 32 * 32;
    };

    std::size_t ties = 0;
    for (int half = 0; half < 400'000; ++half) {
        const double x = half * 0.5;
        REQUIRE(smf::quantise(x) == half_away(x));
        if (half % 2 == 1) {
            ++ties;
        }
    }
    CHECK(ties == 200'000);
}

TEST_CASE("a controller timeline holds its value between breakpoints", "[midi]")
{
    ControllerTimeline timeline;
    timeline.add(100, 7);
    timeline.add(200, 42);

    // Zero-order hold, and the fallback applies before the first breakpoint.
    CHECK(timeline.value_at(0, 99) == 99);
    CHECK(timeline.value_at(99, 99) == 99);
    CHECK(timeline.value_at(100, 99) == 7);
    CHECK(timeline.value_at(150, 99) == 7);
    CHECK(timeline.value_at(200, 99) == 42);
    CHECK(timeline.value_at(1'000'000, 99) == 42);

    // fill() must agree with value_at() sample for sample, and report whether anything moved.
    std::array<int, 8> filled{};
    CHECK(timeline.fill(96, filled, 99));
    CHECK(filled[0] == 99);
    CHECK(filled[3] == 99);
    CHECK(filled[4] == 7);
    CHECK(filled[7] == 7);

    std::array<int, 4> flat{};
    CHECK_FALSE(timeline.fill(300, flat, 99));
    CHECK(flat[0] == 42);
}

TEST_CASE("a re-strike supersedes a note the pedal is holding", "[midi]")
{
    // The case that cost onestop.mid 24 notes: with the damper down, a note-off is parked. If a
    // re-strike leaves that parked entry in place, the pedal's lift closes the note the player is
    // still holding -- each one cut 20-80 ms after sounding.
    std::vector<MidiEvent> events{
        {0, MidiEventKind::channel, 0xB0, 64, 127, {}},  // damper down
        {32, MidiEventKind::channel, 0x90, 60, 100, {}}, // note on
        {64, MidiEventKind::channel, 0x80, 60, 0, {}},   // note off -- parked by the pedal
        {96, MidiEventKind::channel, 0x90, 60, 100, {}}, // re-strike
        {320, MidiEventKind::channel, 0xB0, 64, 0, {}},  // pedal lifts
    };

    const Sequence sequence = sequence_builder::build(events);

    REQUIRE(sequence.notes.size() == 2);

    // The first strike closes when the re-strike arrives, not when the pedal lifts.
    CHECK(sequence.notes[0].on == 32);
    CHECK(sequence.notes[0].off == 96);

    // The second is still sounding when the pedal lifts, and closes there.
    CHECK(sequence.notes[1].on == 96);
    CHECK(sequence.notes[1].off == 320);
}

TEST_CASE("a note-off with the damper down waits for the lift", "[midi]")
{
    std::vector<MidiEvent> events{
        {0, MidiEventKind::channel, 0x90, 60, 100, {}},
        {32, MidiEventKind::channel, 0xB0, 64, 127, {}},
        {64, MidiEventKind::channel, 0x80, 60, 0, {}},
        {640, MidiEventKind::channel, 0xB0, 64, 0, {}},
    };

    const Sequence sequence = sequence_builder::build(events);
    REQUIRE(sequence.notes.size() == 1);
    CHECK(sequence.notes[0].on == 0);
    CHECK(sequence.notes[0].off == 640);
}

TEST_CASE("note parameters are latched at note-on", "[midi]")
{
    // A controller moving during a note must not reach back and change it: the engine reads these
    // once, when the voice starts.
    std::vector<MidiEvent> events{
        {0, MidiEventKind::channel, 0xC0, 42, 0, {}},    // program 42
        {0, MidiEventKind::channel, 0xB0, 7, 90, {}},    // volume 90
        {32, MidiEventKind::channel, 0x90, 60, 100, {}}, // note on
        {64, MidiEventKind::channel, 0xC0, 7, 0, {}},    // program changes mid-note
        {64, MidiEventKind::channel, 0xB0, 7, 10, {}},   // so does volume
        {320, MidiEventKind::channel, 0x80, 60, 0, {}},
    };

    const Sequence sequence = sequence_builder::build(events);
    REQUIRE(sequence.notes.size() == 1);
    CHECK(sequence.notes[0].program == 42);
    CHECK(sequence.notes[0].volume == 90);
}

TEST_CASE("CC#10 cannot reach the random pan position", "[midi]")
{
    // Zero is stored as one: only the GS SysEx panpot writes a true zero, which is what RND is.
    std::vector<MidiEvent> events{
        {0, MidiEventKind::channel, 0xB0, 10, 0, {}},
        {32, MidiEventKind::channel, 0x90, 60, 100, {}},
        {320, MidiEventKind::channel, 0x80, 60, 0, {}},
    };

    const Sequence sequence = sequence_builder::build(events);
    REQUIRE(sequence.notes.size() == 1);
    CHECK(sequence.notes[0].pan == 1);
}

TEST_CASE("a GS block number is not a MIDI channel", "[midi]")
{
    // Block 0 is channel 9 -- the drum part -- blocks 1-9 are channels 0-8, and 10-15 map through.
    CHECK(sequence_builder::channel_from_block(0) == 9);
    CHECK(sequence_builder::channel_from_block(1) == 0);
    CHECK(sequence_builder::channel_from_block(9) == 8);
    CHECK(sequence_builder::channel_from_block(10) == 10);
    CHECK(sequence_builder::channel_from_block(15) == 15);
}

TEST_CASE("data entry commits to whichever of RPN or NRPN was selected last", "[midi]")
{
    // Without tracking which was selected, the drum parameters land on the bend range instead.
    std::vector<MidiEvent> events{
        // Select RPN 00/00 (bend range) and set it to 12.
        {0, MidiEventKind::channel, 0xB0, 101, 0, {}},
        {0, MidiEventKind::channel, 0xB0, 100, 0, {}},
        {0, MidiEventKind::channel, 0xB0, 6, 12, {}},
        // Now select NRPN 0x18 (drum coarse pitch) for key 60 and set it.
        {32, MidiEventKind::channel, 0xB0, 99, 0x18, {}},
        {32, MidiEventKind::channel, 0xB0, 98, 60, {}},
        {32, MidiEventKind::channel, 0xB0, 6, 0x44, {}},
        {64, MidiEventKind::channel, 0x90, 60, 100, {}},
        {320, MidiEventKind::channel, 0x80, 60, 0, {}},
    };

    const Sequence sequence = sequence_builder::build(events);
    REQUIRE(sequence.notes.size() == 1);

    // The NRPN reached the drum key, four steps above centre...
    CHECK(sequence.notes[0].drum_pitch == 4);
    // ...and the bend range kept the value the RPN gave it.
    CHECK(sequence.parts[0].bend_range.value_at(320, 2) == 12);
}
