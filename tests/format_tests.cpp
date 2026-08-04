#include "tabulasonora/midi_formats.hpp"
#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/smf_reader.hpp"
#include "tabulasonora/tone_generator.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

using namespace ts;

// The foreign-format converters, the loop scanners and the EMIDI filter, ported from
// spessasynth_core_c. The converter tests build the smallest well-formed file of each format and
// assert on the parsed events -- the converters serialise SMF and `smf::parse` reads it back, so
// these cover the round trip. The loop and EMIDI tests build plain SMF files, because the
// scanners run on whatever the parse sees regardless of the container it came from.

namespace {

/// An MTrk chunk holding `data` plus the End of Track.
[[nodiscard]] std::vector<std::uint8_t> track_of(std::vector<std::uint8_t> data)
{
    data.insert(data.end(), {0x00, 0xFF, 0x2F, 0x00});
    const auto length = static_cast<std::uint32_t>(data.size());
    std::vector<std::uint8_t> out{'M', 'T', 'r', 'k',
                                  static_cast<std::uint8_t>(length >> 24),
                                  static_cast<std::uint8_t>(length >> 16),
                                  static_cast<std::uint8_t>(length >> 8),
                                  static_cast<std::uint8_t>(length)};
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

/// A whole SMF from track bodies, at the given division.
[[nodiscard]] std::vector<std::uint8_t>
file_of(std::uint16_t division, std::initializer_list<std::vector<std::uint8_t>> bodies,
        std::uint16_t format = 1)
{
    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6};
    file.insert(file.end(),
                {static_cast<std::uint8_t>(format >> 8), static_cast<std::uint8_t>(format),
                 0, static_cast<std::uint8_t>(bodies.size()),
                 static_cast<std::uint8_t>(division >> 8), static_cast<std::uint8_t>(division)});
    for (const auto& body : bodies) {
        const auto bytes = track_of(body);
        file.insert(file.end(), bytes.begin(), bytes.end());
    }
    return file;
}

/// A two-byte variable-length delta for tick values up to 16383.
[[nodiscard]] std::vector<std::uint8_t> delta(int ticks)
{
    if (ticks < 0x80) {
        return {static_cast<std::uint8_t>(ticks)};
    }
    return {static_cast<std::uint8_t>(0x80 | (ticks >> 7)),
            static_cast<std::uint8_t>(ticks & 0x7F)};
}

/// Appends `tail` to `head`, for building track bodies piecewise.
void append(std::vector<std::uint8_t>& head, const std::vector<std::uint8_t>& tail)
{
    head.insert(head.end(), tail.begin(), tail.end());
}

/// A marker meta event carrying `text`.
[[nodiscard]] std::vector<std::uint8_t> marker(const std::string& text)
{
    std::vector<std::uint8_t> out{0xFF, 0x06, static_cast<std::uint8_t>(text.size())};
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

/// Only the channel-voice events; tempo and markers never reach the event list.
[[nodiscard]] std::vector<MidiEvent> channel_events(const std::vector<MidiEvent>& events)
{
    std::vector<MidiEvent> out;
    std::copy_if(events.begin(), events.end(), std::back_inserter(out),
                 [](const MidiEvent& event) { return event.kind == MidiEventKind::channel; });
    return out;
}

} // namespace

// ── Format converters ─────────────────────────────────────────────────────────────────────────

TEST_CASE("a MUS score converts and parses", "[format][mus]")
{
    // The smallest well-formed MUS: one instrument, a note-on for channel 0, a note-on for the
    // drum channel (MUS channel 15, which must land on MIDI channel 9), and the score end.
    const std::vector<std::uint8_t> score{
        0x10, 0xBC, 0x64, // note play, channel 0: note 60 with the velocity flag, velocity 100
        0x1F, 0xA3, 0x40, // note play, channel 15: note 35, velocity 64
        0x60,             // score end
    };

    std::vector<std::uint8_t> file{'M', 'U', 'S', 0x1A};
    const auto length = static_cast<std::uint16_t>(score.size());
    file.insert(file.end(),
                {static_cast<std::uint8_t>(length & 0xFF), static_cast<std::uint8_t>(length >> 8),
                 18, 0,      // score offset
                 1, 0,       // primary channels
                 0, 0,       // secondary channels
                 1, 0,       // instrument count
                 0, 0,       // reserved
                 0, 0});     // the one instrument entry
    append(file, score);
    file.resize(0x20, 0); // detection requires the reference's minimum size

    const std::vector<MidiEvent> events = smf::parse(file, 32000);

    REQUIRE(events.size() == 2);
    CHECK(events[0].status == 0x90);
    CHECK(events[0].data1 == 60);
    CHECK(events[0].data2 == 100);
    CHECK(events[1].status == 0x99); // MUS channel 15 is the drums
    CHECK(events[1].data1 == 35);
    CHECK(events[1].data2 == 64);
}

TEST_CASE("a MIDS segment converts and parses", "[format][mids]")
{
    // One segment holding one packed note-on, in the 8-byte record layout (flags bit 0).
    const auto record_event = static_cast<std::uint32_t>((100 << 16) | (60 << 8) | 0x93);
    std::vector<std::uint8_t> file{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'M', 'I', 'D', 'S',
                                   'f', 'm', 't', ' ', 12, 0, 0, 0,
                                   96, 0, 0, 0,  // time division
                                   0, 0, 0, 0,   // max buffer
                                   1, 0, 0, 0,   // flags: 8-byte records
                                   'd', 'a', 't', 'a'};
    const auto le32 = [](std::uint32_t value) {
        return std::vector<std::uint8_t>{static_cast<std::uint8_t>(value & 0xFF),
                                         static_cast<std::uint8_t>((value >> 8) & 0xFF),
                                         static_cast<std::uint8_t>((value >> 16) & 0xFF),
                                         static_cast<std::uint8_t>(value >> 24)};
    };
    append(file, le32(4 + 4 + 4 + 8)); // data chunk size
    append(file, le32(1));             // segment count
    append(file, le32(0));             // unused segment header word
    append(file, le32(8));             // segment size
    append(file, le32(0));             // record delta
    append(file, le32(record_event));
    const auto riff_size = static_cast<std::uint32_t>(file.size() - 8);
    file[4] = static_cast<std::uint8_t>(riff_size & 0xFF);
    file[5] = static_cast<std::uint8_t>((riff_size >> 8) & 0xFF);

    const std::vector<MidiEvent> events = smf::parse(file, 32000);

    REQUIRE(events.size() == 1);
    CHECK(events[0].status == 0x93);
    CHECK(events[0].data1 == 60);
    CHECK(events[0].data2 == 100);
}

TEST_CASE("an XMI track synthesises its note-offs", "[format][xmi]")
{
    // XMI stores a duration on every note instead of note-offs. One note of duration 40 at 60
    // TPQN under the default tempo: half a beat, a quarter second, 8000 samples at 32 kHz.
    const std::vector<std::uint8_t> events_body{
        0x02,                   // delta 2
        0x90, 0x3C, 0x64, 0x28, // note 60, velocity 100, duration 40
        0xFF, 0x2F,             // end of track -- no length byte in XMI
    };

    std::vector<std::uint8_t> file{'F', 'O', 'R', 'M', 0, 0, 0, 14, 'X', 'D', 'I', 'R'};
    file.resize(8 + 14, 0); // pad the XDIR body

    std::vector<std::uint8_t> form{'F', 'O', 'R', 'M', 0, 0, 0, 0, 'X', 'M', 'I', 'D',
                                   'E', 'V', 'N', 'T', 0, 0, 0,
                                   static_cast<std::uint8_t>(events_body.size())};
    append(form, events_body);
    const auto form_size = static_cast<std::uint32_t>(form.size() - 8);
    form[7] = static_cast<std::uint8_t>(form_size);

    std::vector<std::uint8_t> cat{'C', 'A', 'T', ' ', 0, 0, 0,
                                  static_cast<std::uint8_t>(4 + form.size()),
                                  'X', 'M', 'I', 'D'};
    append(cat, form);
    append(file, cat);

    const std::vector<MidiEvent> events = smf::parse(file, 32000);

    REQUIRE(events.size() == 2);
    CHECK(events[0].status == 0x90);
    CHECK(events[0].data2 == 100);
    CHECK(events[1].status == 0x90);
    CHECK(events[1].data2 == 0); // the synthesised note-off
    // Tick 2 and tick 42 at 60 TPQN under the default tempo, each quantised to the block grid.
    CHECK(events[0].position == 512);
    CHECK(events[1].position == 11200);
}

TEST_CASE("a RIFF-MIDI wrapper unwraps to its payload", "[format][rmidi]")
{
    const std::vector<std::uint8_t> inner =
        file_of(480, {{0x00, 0xC0, 0x30, 0x00, 0x90, 0x3C, 0x64}});

    std::vector<std::uint8_t> file{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'R', 'M', 'I', 'D',
                                   'd', 'a', 't', 'a'};
    const auto inner_size = static_cast<std::uint32_t>(inner.size());
    file.insert(file.end(),
                {static_cast<std::uint8_t>(inner_size & 0xFF),
                 static_cast<std::uint8_t>((inner_size >> 8) & 0xFF),
                 static_cast<std::uint8_t>((inner_size >> 16) & 0xFF),
                 static_cast<std::uint8_t>(inner_size >> 24)});
    append(file, inner);
    const auto riff_size = static_cast<std::uint32_t>(file.size() - 8);
    file[4] = static_cast<std::uint8_t>(riff_size & 0xFF);
    file[5] = static_cast<std::uint8_t>((riff_size >> 8) & 0xFF);

    const std::vector<MidiEvent> wrapped = smf::parse(file, 32000);
    const std::vector<MidiEvent> direct = smf::parse(inner, 32000);

    REQUIRE(wrapped.size() == direct.size());
    for (std::size_t i = 0; i < wrapped.size(); ++i) {
        CHECK(wrapped[i].status == direct[i].status);
        CHECK(wrapped[i].data1 == direct[i].data1);
        CHECK(wrapped[i].position == direct[i].position);
    }
}

TEST_CASE("a GMF file gains its conductor and keeps its events", "[format][gmf]")
{
    // Detection requires 32 bytes, and the stream must fill them itself: trailing padding would
    // be read as running-status events, exactly as the reference's track parser reads it.
    std::vector<std::uint8_t> file{'G', 'M', 'F', 0x01, 0x00, 0x05, 0x00}; // tempo 5 * 100000
    append(file, {0x00, 0xC0, 0x30,
                  0x00, 0x90, 0x3C, 0x64, 0x10, 0x80, 0x3C, 0x00,
                  0x00, 0x90, 0x3E, 0x64, 0x10, 0x80, 0x3E, 0x00,
                  0x00, 0x90, 0x40, 0x64, 0x10, 0x80, 0x40, 0x00});
    REQUIRE(file.size() >= 32);

    const std::vector<MidiEvent> events = smf::parse(file, 32000);

    // The conductor's GS-style reset SysEx, then the raw stream's program change and notes.
    REQUIRE(events.size() == 8);
    CHECK(events[0].kind == MidiEventKind::sysex);
    REQUIRE_FALSE(events[0].sysex.empty());
    CHECK(events[0].sysex.front() == 0xF0);
    CHECK(events[1].status == 0xC0);
    CHECK(events[1].data1 == 0x30);
    CHECK(events[2].status == 0x90);
}

TEST_CASE("an XMF tree yields its embedded Standard MIDI File", "[format][xmf]")
{
    const std::vector<std::uint8_t> inner =
        file_of(480, {{0x00, 0x90, 0x3C, 0x64}});

    // One FileNode carrying the SMF inline and uncompressed: resource-format metadata (field 3,
    // standard type, SMF format 0), an empty unpackers block, an inline reference, the payload.
    std::vector<std::uint8_t> node{
        0,          // node length -- patched below
        0,          // item count: a FileNode
        12,         // header size: body starts at the reference type
        7,          // metadata length
        0, 3,       // field specifier: standard field 3, ResourceFormat
        0,          // international contents: none
        3,          // universal contents length
        4, 0, 0,    // binary format id, then [standard, SMF type 0]
        0,          // unpackers: none
        1,          // reference type: inline
    };
    node[0] = static_cast<std::uint8_t>(node.size() + inner.size());
    append(node, inner);

    std::vector<std::uint8_t> file{'X', 'M', 'F', '_', '1', '.', '0', '0',
                                   0,   // file length -- informational
                                   0,   // metadata table: none
                                   11}; // tree start: right here
    append(file, node);

    const std::vector<MidiEvent> events = smf::parse(file, 32000);

    REQUIRE(events.size() == 1);
    CHECK(events[0].status == 0x90);
    CHECK(events[0].data1 == 0x3C);
}

// ── Loop scanning ─────────────────────────────────────────────────────────────────────────────
//
// Division 500 under the default 500000 us/beat tempo makes the arithmetic legible: one tick is
// a millisecond, so tick 1000 is 32000 samples at 32 kHz.

TEST_CASE("marker meta events declare a loop", "[format][loop]")
{
    std::vector<std::uint8_t> body{0x00, 0x90, 0x3C, 0x64};
    append(body, delta(500));
    append(body, marker(" LoopStart ")); // case and padding must not matter
    append(body, delta(500));
    append(body, {0x90, 0x40, 0x64});
    append(body, delta(1000));
    append(body, marker("loopEnd"));

    const smf::Song song = smf::load(file_of(500, {std::move(body)}), 32000);

    REQUIRE(song.loop.has_value());
    CHECK(song.loop->start == 16000); // tick 500
    CHECK(song.loop->end == 64000);   // tick 2000
    CHECK_FALSE(song.loop->soft);     // markers never make a soft loop
}

TEST_CASE("a start-only loop runs to the last voice event", "[format][loop]")
{
    std::vector<std::uint8_t> body{0x00, 0x90, 0x3C, 0x64};
    append(body, delta(500));
    append(body, marker("loopStart"));
    append(body, delta(1500));
    append(body, {0x80, 0x3C, 0x00}); // the last voice event, at tick 2000

    const smf::Song song = smf::load(file_of(500, {std::move(body)}), 32000);

    REQUIRE(song.loop.has_value());
    CHECK(song.loop->start == 16000);
    CHECK(song.loop->end == 64000);
}

TEST_CASE("a loop start on the song's final tick is no loop at all", "[format][loop]")
{
    std::vector<std::uint8_t> body{0x00, 0x90, 0x3C, 0x64};
    append(body, delta(2000));
    append(body, {0x80, 0x3C, 0x00});
    append(body, {0x00});
    append(body, marker("loopStart"));

    const smf::Song song = smf::load(file_of(500, {std::move(body)}), 32000);

    CHECK_FALSE(song.loop.has_value());
}

TEST_CASE("the XMI controller pair declares a soft loop", "[format][loop]")
{
    std::vector<std::uint8_t> body{0x00, 0xB0, 116, 0x00, 0x00, 0x90, 0x3C, 0x64};
    append(body, delta(1000));
    append(body, {0xB0, 117, 0x00});

    const smf::Song song = smf::load(file_of(500, {std::move(body)}), 32000);

    REQUIRE(song.loop.has_value());
    CHECK(song.loop->start == 0);
    CHECK(song.loop->end == 32000);
    CHECK(song.loop->soft); // the file marked its own end
}

TEST_CASE("the Touhou controller pair only counts in a format-0 file", "[format][loop]")
{
    std::vector<std::uint8_t> body{0x00, 0x90, 0x3C, 0x64};
    append(body, delta(500));
    append(body, {0xB0, 2, 0x00});
    append(body, delta(500));
    append(body, {0xB0, 4, 0x00});
    append(body, delta(1000));
    append(body, {0x80, 0x3C, 0x00});
    const std::vector<std::uint8_t> copy = body;

    const smf::Song format0 = smf::load(file_of(500, {std::move(body)}, 0), 32000);
    REQUIRE(format0.loop.has_value());
    CHECK(format0.loop->start == 16000);
    CHECK(format0.loop->end == 32000);
    CHECK(format0.loop->soft);

    const smf::Song format1 = smf::load(file_of(500, {copy}, 1), 32000);
    CHECK_FALSE(format1.loop.has_value());
}

TEST_CASE("a non-zero Touhou controller voids that scan", "[format][loop]")
{
    std::vector<std::uint8_t> body{0x00, 0xB0, 2, 0x00};
    append(body, delta(500));
    append(body, {0xB0, 4, 0x07}); // a non-zero value on the pair is not a loop marker
    append(body, delta(500));
    append(body, {0x90, 0x3C, 0x64});

    const smf::Song song = smf::load(file_of(500, {std::move(body)}, 0), 32000);

    CHECK_FALSE(song.loop.has_value());
}

TEST_CASE("RPG Maker's controller declares a loop unless EMIDI is present", "[format][loop]")
{
    std::vector<std::uint8_t> body{0x00, 0x90, 0x3C, 0x64};
    append(body, delta(500));
    append(body, {0xB0, 111, 0x00});
    append(body, delta(1500));
    append(body, {0x80, 0x3C, 0x00});
    const std::vector<std::uint8_t> copy = body;

    const smf::Song plain = smf::load(file_of(500, {std::move(body)}), 32000);
    REQUIRE(plain.loop.has_value());
    CHECK(plain.loop->start == 16000);

    // The same file with an EMIDI track designation anywhere: CC 111 means something else to
    // EMIDI, so the loop scan must not trust it.
    const smf::Song gated =
        smf::load(file_of(500, {copy, {0x00, 0xB1, 110, 0x00, 0x00, 0x91, 0x40, 0x64}}), 32000);
    CHECK_FALSE(gated.loop.has_value());
}

// ── EMIDI track filtering ─────────────────────────────────────────────────────────────────────

TEST_CASE("tracks designated for a non-GM synthesizer are dropped", "[format][emidi]")
{
    // Track one is designated GM (127 plays everywhere); track two is designated for device 2 --
    // an MT-32-style target -- and duplicates the material. Playing both as GM doubles the
    // voices, which is exactly what the filter exists to stop.
    const std::vector<std::uint8_t> gm_track{0x00, 0xB0, 110, 127, 0x00, 0x90, 0x3C, 0x64};
    const std::vector<std::uint8_t> other_track{0x00, 0xB1, 110, 2, 0x00, 0x91, 0x3C, 0x64};

    const std::vector<MidiEvent> events =
        channel_events(smf::parse(file_of(480, {gm_track, other_track}), 32000));

    REQUIRE(events.size() == 2); // the designation itself and the one surviving note
    CHECK(events[0].status == 0xB0);
    CHECK(events[0].data1 == 110);
    CHECK(events[1].status == 0x90);
    CHECK(events[1].channel() == 0);
}

TEST_CASE("a dropped track's loop points are dropped with it", "[format][emidi]")
{
    // The non-GM track carries the only loop markers. The reference rescans loops after the
    // filter, so they must not survive here either.
    const std::vector<std::uint8_t> gm_track{0x00, 0xB0, 110, 127, 0x00, 0x90, 0x3C, 0x64};
    std::vector<std::uint8_t> other_track{0x00, 0xB1, 110, 2, 0x00, 0xB1, 116, 0x00};
    append(other_track, delta(1000));
    append(other_track, {0xB1, 117, 0x00});

    const smf::Song song = smf::load(file_of(480, {gm_track, std::move(other_track)}), 32000);

    CHECK_FALSE(song.loop.has_value());
}

// ── Loop playback ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The looping test file: a one-second body between loopStart at 500 ms and loopEnd at 1500 ms.
[[nodiscard]] smf::Song looped_song()
{
    std::vector<std::uint8_t> body{0x00, 0xC0, 0x30, 0x00, 0x90, 0x3C, 0x64};
    append(body, delta(500));
    append(body, marker("loopStart"));
    append(body, delta(500));
    append(body, {0x80, 0x3C, 0x00, 0x00, 0x90, 0x40, 0x64});
    append(body, delta(500));
    append(body, {0x80, 0x40, 0x00});
    append(body, delta(0));
    append(body, marker("loopEnd"));
    return smf::load(file_of(500, {std::move(body)}), 32000);
}

} // namespace

TEST_CASE("an infinite loop jumps back and keeps playing", "[format][loop][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    SequencePlayer player{generator, looped_song()};
    REQUIRE(player.loop().has_value());
    CHECK(player.loop()->start == 16000);
    CHECK(player.loop()->end == 48000);

    player.set_loop_count(-1);

    // Three seconds of audio over a song whose single pass is a second and a half: without the
    // jumps this would run out, with them the position never passes the loop end.
    std::vector<float> left(32000 * 3);
    std::vector<float> right(32000 * 3);
    player.render(left, right);

    CHECK(player.loops_played() >= 1);
    CHECK(player.position() <= player.loop()->end);
    CHECK_FALSE(player.at_end());
}

TEST_CASE("a finite loop count renders its passes and fades out", "[format][loop][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    SequencePlayer player{generator, looped_song()};
    player.set_loop_count(2);
    player.set_fade_seconds(1.0);

    const RenderResult result = player.render_to_end(/*tail_seconds=*/0.5);

    // First pass to the loop end, one more body, the fade, the tail.
    const std::size_t expected = 48000 + 32000 + 32000 + 16000;
    REQUIRE(result.left.size() == expected);
    CHECK(player.at_end());

    // The fade ran to silence: the final stretch carries nothing.
    float tail_peak = 0.0F;
    for (std::size_t i = expected - 8000; i < expected; ++i) {
        tail_peak = std::max({tail_peak, std::abs(result.left[i]), std::abs(result.right[i])});
    }
    CHECK(tail_peak == 0.0F);
}
