#include "tabulasonora/sequence.hpp"
#include "tabulasonora/tone_generator.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <vector>

using namespace ts;

// The second MIDI port, and the sixteen parts it reaches.
//
// The module allocates thirty-two parts and addresses them as `port * 16 + channel`, but
// `midi_drain_ready_to_ports` masks the port field out of every packet on the way to the FIFO
// (`and r8b,0Fh`), so a stock DLL can only ever reach the first sixteen. Widening that mask to
// `0x1f` keeps the low bit of the port and admits exactly two — which is what these pin.
//
// They assert on part state rather than audio because part selection is all the port decides: the
// DSP under it is the same objects either way, and the block-loop tests already cover that.

namespace {

constexpr int channels = Sequence::channel_count;

/// A USB-MIDI Event Packet: `(port << 4) | class`, then the message least-significant first.
[[nodiscard]] constexpr std::uint32_t packet(int port, int status, int data1, int data2)
{
    return static_cast<std::uint32_t>(((port & 0xF) << 4) | (status << 8) | (data1 << 16)
                                      | (data2 << 24));
}

/// A GS delay-send write: F0 41 10 42 12 40 1n 2C vv sum F7.
[[nodiscard]] std::vector<std::uint8_t> delay_send(int block, int value)
{
    std::vector<std::uint8_t> message{0xF0,
                                      0x41,
                                      0x10,
                                      0x42,
                                      0x12,
                                      0x40,
                                      static_cast<std::uint8_t>(0x10 | block),
                                      0x2C,
                                      static_cast<std::uint8_t>(value),
                                      0x00,
                                      0xF7};
    int sum = 0;
    for (std::size_t i = 5; i < 9; ++i) {
        sum += message[i];
    }
    message[9] = static_cast<std::uint8_t>((128 - (sum & 0x7F)) & 0x7F);
    return message;
}

} // namespace

TEST_CASE("thirty-two parts across two ports", "[port]")
{
    STATIC_CHECK(ToneGenerator::port_count == 2);
    STATIC_CHECK(ToneGenerator::part_count == 32);
}

TEST_CASE("port B drives the second sixteen parts", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    generator.send_channel(1, 0xC0, 48, 0);
    generator.send_channel(1, 0xB0, 7, 40);

    CHECK(generator.part(channels).program == 48);
    CHECK(generator.part(channels).volume() == 40);

    // The same channel on port A is untouched, which is the whole point of the second port.
    CHECK(generator.part(0).program != 48);
    CHECK(generator.part(0).volume() != 40);
}

TEST_CASE("a portless caller gets port A", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    generator.send_channel(0xC0 | 3, 48, 0);

    CHECK(generator.part(3).program == 48);
    CHECK(generator.part(channels + 3).program != 48);
}

TEST_CASE("ports above the second fold onto the two that exist", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // 0x1f keeps the low bit of the port and nothing above it, so even ports land on A and odd ones
    // on B rather than indexing parts that were never allocated.
    generator.send_channel(2, 0xC0, 48, 0);
    generator.send_channel(3, 0xC0, 52, 0);

    CHECK(generator.part(0).program == 48);
    CHECK(generator.part(channels).program == 52);
}

TEST_CASE("a packet carries its own port", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    generator.send_packet(packet(1, 0xC0 | 5, 48, 0));
    generator.send_packet(packet(0, 0xC0 | 5, 52, 0));

    CHECK(generator.part(5).program == 52);
    CHECK(generator.part(channels + 5).program == 48);
}

TEST_CASE("nothing latches the port between messages", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // The module dispatches on each packet's own port field as that packet drains, so a message
    // sent to port B does not leave the engine "on" port B for the one after it.
    generator.send_channel(1, 0xC0, 48, 0);
    generator.send_channel(0xC0, 52, 0);

    CHECK(generator.part(channels).program == 48);
    CHECK(generator.part(0).program == 52);
}

TEST_CASE("each port has its own drum kit", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // Two programs the kit table actually distinguishes; asserting on a pair it maps to the same
    // kit would pass whether or not the ports were separate.
    const int row = generator.effective_drum_map_row();
    const auto defined = [&](int program) { return notes.drums().kit_for_program(program, row); };

    int first = -1;
    int second = -1;
    for (int program = 0; program < 128; ++program) {
        const std::optional<int> kit = defined(program);
        if (!kit) {
            continue;
        }
        if (first < 0) {
            first = program;
        } else if (*kit != *defined(first)) {
            second = program;
            break;
        }
    }
    REQUIRE(first >= 0);
    REQUIRE(second >= 0);

    generator.send_channel(0, 0xC0 | 9, first, 0);
    generator.send_channel(1, 0xC0 | 9, second, 0);

    CHECK(generator.drum_kit_for(0) == *defined(first));
    CHECK(generator.drum_kit_for(1) == *defined(second));

    // drum_kit() without a port still means port A.
    CHECK(generator.drum_kit() == generator.drum_kit_for(0));
}

TEST_CASE("GS part addressing is relative to the arriving port", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    // Delay send on GS block 2, which is channel 1. The same address names channel 1 of whichever
    // port the message came in on.
    const std::vector<std::uint8_t> message = delay_send(2, 40);
    generator.send_sysex(1, message);

    CHECK(generator.part(channels + 1).delay_send == 40);
    CHECK(generator.part(1).delay_send != 40);
}

TEST_CASE("reset clears both ports", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    generator.send_channel(0, 0xB0, 7, 40);
    generator.send_channel(1, 0xB0, 7, 40);
    generator.reset();

    CHECK(generator.part(0).volume() != 40);
    CHECK(generator.part(channels).volume() != 40);
}

TEST_CASE("sixty-four parts are reachable and independent", "[port][sccore]")
{
    // An extension past the module, which has thirty-two. What has to hold is that the extra ports
    // are real parts and not aliases of the first two: the index is formed by masking the port
    // field, and a mask one bit too narrow would fold port C onto port A silently.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ToneGeneratorOptions options;
    options.ports = ToneGenerator::max_port_count;
    options.polyphony = 256; // sixty-four parts sharing sixty-four voices would steal without pause
    ToneGenerator generator{notes, options};

    REQUIRE(generator.ports() == 4);
    REQUIRE(generator.parts() == 64);

    // A distinct program on the same channel of every port, then read all four back.
    for (int port = 0; port < generator.ports(); ++port) {
        generator.send_channel(port, 0xC0, 20 + port, 0);
        generator.send_channel(port, 0xB0, 7, 30 + port);
    }
    for (int port = 0; port < generator.ports(); ++port) {
        INFO("port " << port);
        CHECK(generator.part(port * channels).program == 20 + port);
        CHECK(generator.part(port * channels).volume() == 30 + port);
    }

    // And every one of the sixty-four can sound, which the part array being wide enough is only
    // half of -- the note path has to address them too.
    for (int part = 0; part < generator.parts(); ++part) {
        const int channel = part % channels;
        if (channel == 9) {
            continue; // the drum channel of each port takes a different path
        }
        generator.send_channel(part / channels, 0x90 | channel, 60, 100);
    }
    std::vector<float> left(ToneGenerator::block_size);
    std::vector<float> right(ToneGenerator::block_size);
    generator.render(left, right);
    CHECK(generator.note_count() == 60);
    CHECK(generator.active_voices() > 32);
}

TEST_CASE("the default stays at the hardware's two ports", "[port][sccore]")
{
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};
    ToneGenerator generator{notes};

    CHECK(generator.ports() == ToneGenerator::port_count);
    CHECK(generator.parts() == ToneGenerator::part_count);
}

TEST_CASE("a port count that is not a power of two is refused", "[port][sccore]")
{
    // The part index is formed by masking, so three ports would alias rather than fail, and a
    // silent alias is worse than a refusal.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ToneGeneratorOptions options;
    options.ports = 3;
    CHECK_THROWS_AS(ToneGenerator(notes, options), std::invalid_argument);
}

TEST_CASE("a track's MIDI Port meta routes its events", "[port][smf]")
{
    // FF 21 is how a file addresses more than sixteen channels. It is a prefix, so it applies to
    // the events that follow it rather than to the track as a whole -- a track that switches
    // partway has to be honoured, not flattened onto one port.
    const auto track = [](int port, std::initializer_list<std::uint8_t> body) {
        std::vector<std::uint8_t> out{'M', 'T', 'r', 'k', 0, 0, 0, 0};
        std::vector<std::uint8_t> data{0x00, 0xFF, 0x21, 0x01, static_cast<std::uint8_t>(port)};
        data.insert(data.end(), body);
        data.insert(data.end(), {0x00, 0xFF, 0x2F, 0x00});
        const auto length = static_cast<std::uint32_t>(data.size());
        out[4] = static_cast<std::uint8_t>(length >> 24);
        out[5] = static_cast<std::uint8_t>(length >> 16);
        out[6] = static_cast<std::uint8_t>(length >> 8);
        out[7] = static_cast<std::uint8_t>(length);
        out.insert(out.end(), data.begin(), data.end());
        return out;
    };

    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 3, 0x01, 0xE0};
    for (int port = 0; port < 3; ++port) {
        // Each track sends a program change on channel 0, with a different program per port.
        const auto bytes = track(port, {0x00, 0xC0, static_cast<std::uint8_t>(40 + port)});
        file.insert(file.end(), bytes.begin(), bytes.end());
    }

    const std::vector<MidiEvent> events = smf::parse(file, 32000);
    REQUIRE(events.size() == 3);
    for (int port = 0; port < 3; ++port) {
        INFO("port " << port);
        CHECK(events[static_cast<std::size_t>(port)].port == port);
        CHECK(events[static_cast<std::size_t>(port)].data1 == 40 + port);
    }
}

TEST_CASE("an untagged file is all port zero", "[port][smf][sccore]")
{
    // The behaviour every existing file relies on, asserted so port tagging cannot quietly change
    // it: no FF 21 and no FF 09 means port 0 throughout.
    const std::filesystem::path path = testdata::repository_root() / "testdata" / "canyon.mid";
    if (!std::filesystem::exists(path)) {
        SKIP("canyon.mid is not in testdata");
    }

    const std::vector<MidiEvent> events = smf::read(path, 32000);
    REQUIRE_FALSE(events.empty());
    CHECK(std::all_of(events.begin(), events.end(),
                      [](const MidiEvent& event) { return event.port == 0; }));
}

TEST_CASE("Africa.mid is tagged across four ports", "[port][smf][sccore]")
{
    // A real four-port file: 31 tracks, FF 21 throughout, and -- the reason 64 parts exist -- four
    // separate drum tracks, one on channel 9 of each port.
    const std::filesystem::path path = testdata::repository_root() / "testdata" / "Africa.mid";
    if (!std::filesystem::exists(path)) {
        SKIP("Africa.mid is not in testdata");
    }

    const std::vector<MidiEvent> events = smf::read(path, 32000);
    REQUIRE_FALSE(events.empty());

    std::array<int, 4> per_port{};
    std::array<bool, 4> drums_on_port{};
    for (const MidiEvent& event : events) {
        if (event.port < 0 || event.port >= 4) {
            continue;
        }
        ++per_port[static_cast<std::size_t>(event.port)];
        if (event.kind == MidiEventKind::channel && event.message_type() == 0x90
            && event.channel() == 9) {
            drums_on_port[static_cast<std::size_t>(event.port)] = true;
        }
    }

    for (int port = 0; port < 4; ++port) {
        INFO("port " << port);
        CHECK(per_port[static_cast<std::size_t>(port)] > 0);
        CHECK(drums_on_port[static_cast<std::size_t>(port)]);
    }
}

TEST_CASE("a port tag does not leak into the next track", "[port][smf]")
{
    // The tag belongs to the track that carries it. If it survived into the next track, a file
    // whose first track is tagged would drag every later untagged track along with it -- and
    // untagged tracks are the common case even in multi-port files.
    const auto build = [](std::initializer_list<std::vector<std::uint8_t>> bodies) {
        std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0,   0, 0, 6,
                                       0,   1,   0,   0,   0x01, 0xE0};
        file[11] = static_cast<std::uint8_t>(bodies.size());
        for (const auto& body : bodies) {
            std::vector<std::uint8_t> data = body;
            data.insert(data.end(), {0x00, 0xFF, 0x2F, 0x00});
            const auto length = static_cast<std::uint32_t>(data.size());
            file.insert(file.end(), {'M', 'T', 'r', 'k',
                                     static_cast<std::uint8_t>(length >> 24),
                                     static_cast<std::uint8_t>(length >> 16),
                                     static_cast<std::uint8_t>(length >> 8),
                                     static_cast<std::uint8_t>(length)});
            file.insert(file.end(), data.begin(), data.end());
        }
        return file;
    };

    // Track 0 tags port 2; track 1 tags nothing at all.
    const std::vector<MidiEvent> events =
        smf::parse(build({{0x00, 0xFF, 0x21, 0x01, 0x02, 0x00, 0xC0, 0x30},
                          {0x00, 0xC1, 0x31}}),
                   32000);

    REQUIRE(events.size() == 2);
    CHECK(events[0].port == 2);
    CHECK(events[1].port == 0);
}

TEST_CASE("two tracks sharing a port and channel still address one part", "[port][smf]")
{
    // The converse of the leak: Africa.mid has pairs of tracks on the same port and channel -- two
    // "F Horns" tracks on port 0 channel 8, two "Piano" tracks on port 1 channel 1. Those are one
    // part layered from two tracks, and the port must not invent a distinction between them.
    const auto track = [](std::vector<std::uint8_t> data) {
        data.insert(data.end(), {0x00, 0xFF, 0x2F, 0x00});
        const auto length = static_cast<std::uint32_t>(data.size());
        std::vector<std::uint8_t> out{'M', 'T', 'r', 'k',
                                      static_cast<std::uint8_t>(length >> 24),
                                      static_cast<std::uint8_t>(length >> 16),
                                      static_cast<std::uint8_t>(length >> 8),
                                      static_cast<std::uint8_t>(length)};
        out.insert(out.end(), data.begin(), data.end());
        return out;
    };

    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x01, 0xE0};
    for (int i = 0; i < 2; ++i) {
        const auto bytes = track({0x00, 0xFF, 0x21, 0x01, 0x01, 0x00, 0x90, 0x3C,
                                  static_cast<std::uint8_t>(100 + i)});
        file.insert(file.end(), bytes.begin(), bytes.end());
    }

    const std::vector<MidiEvent> events = smf::parse(file, 32000);
    REQUIRE(events.size() == 2);
    CHECK(events[0].port == 1);
    CHECK(events[1].port == 1);
    CHECK(events[0].channel() == events[1].channel());
}

TEST_CASE("running status survives a meta event", "[smf]")
{
    // Storing a meta or SysEx status as the running status desyncs a track: the next event that
    // omits its status byte is read as another meta, its length swallowed as data, and everything
    // after it parsed at the wrong offsets. Clearing the running status instead is what the spec
    // says, and it is just as wrong in practice -- sequencers write files that resume running
    // status across a meta event, and dropping those notes is not an option either.
    //
    // The track below is exactly that shape: a note with an explicit status, a meta event, then
    // two notes relying on running status.
    std::vector<std::uint8_t> data{
        0x00, 0x90, 0x3C, 0x40,             // note on, explicit status
        0x00, 0xFF, 0x03, 0x02, 'h', 'i',   // a track-name meta in the middle
        0x10, 0x3E, 0x41,                   // running status: another note on
        0x10, 0x40, 0x42,                   // and another
        0x00, 0xFF, 0x2F, 0x00,
    };
    const auto length = static_cast<std::uint32_t>(data.size());
    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                   'M', 'T', 'r', 'k',
                                   static_cast<std::uint8_t>(length >> 24),
                                   static_cast<std::uint8_t>(length >> 16),
                                   static_cast<std::uint8_t>(length >> 8),
                                   static_cast<std::uint8_t>(length)};
    file.insert(file.end(), data.begin(), data.end());

    const std::vector<MidiEvent> events = smf::parse(file, 32000);
    REQUIRE(events.size() == 3);
    for (std::size_t i = 0; i < events.size(); ++i) {
        INFO("event " << i);
        CHECK(events[i].status == 0x90);
        CHECK(events[i].data1 == 0x3C + static_cast<int>(i) * 2);
        CHECK(events[i].data2 == 0x40 + static_cast<int>(i));
    }
}

TEST_CASE("Africa.mid runs its full length", "[smf][sccore]")
{
    // 31 tracks, every one interleaving meta events with running-status notes. Mishandling that
    // cost about a third of every track and ended the song at 90 s instead of 281 -- which sounds
    // like the tempo running away rather than like missing notes, since what is left closes up.
    const std::filesystem::path path = testdata::repository_root() / "testdata" / "Africa.mid";
    if (!std::filesystem::exists(path)) {
        SKIP("Africa.mid is not in testdata");
    }

    const std::vector<MidiEvent> events = smf::read(path, 32000);

    // 211,200 ticks at 480 per quarter and 638,297 us per quarter is 280.85 s.
    const double seconds = static_cast<double>(events.back().position) / 32000.0;
    CHECK(seconds > 280.8);
    CHECK(seconds < 280.9);

    const auto note_ons = std::count_if(events.begin(), events.end(), [](const MidiEvent& event) {
        return event.kind == MidiEventKind::channel && event.message_type() == 0x90
               && event.data2 > 0;
    });
    CHECK(note_ons == 20234);
}
