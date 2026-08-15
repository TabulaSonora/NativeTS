#include "tabulasonora/sequence.hpp"
#include "tabulasonora/tone_generator.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <utility>
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

TEST_CASE("mute and solo address a part, not a channel on every port", "[port][sccore]")
{
    // The mask is sixty-four wide and one entry means one part. It was sixteen wide once, and the
    // fold that made a part index fit it meant muting channel 1 silenced channel 1 on *both* ports
    // -- two parts with different programs, different volumes and, on a multi-port file, different
    // music, standing behind one switch.
    const RomImage rom =
        RomImage::open(testdata::require_sccore().string(), RomVerification::quick);
    NoteRenderer notes{rom};

    ChannelMask mask;
    ToneGeneratorOptions options;
    options.channels = &mask;

    // Only port B sounds. Port A's channel 0 is silent throughout and is only here to be muted.
    const auto render_port_b = [&](auto&& before) {
        ToneGenerator generator{notes, options};
        before();
        generator.send_channel(1, 0xC0, 48, 0);
        generator.send_channel(1, 0x90, 60, 100);

        std::vector<float> left(4096);
        std::vector<float> right(4096);
        generator.render(left, right);
        return std::ranges::max(left | std::views::transform([](float v) { return std::abs(v); }));
    };

    const float unmuted = render_port_b([] {});
    REQUIRE(unmuted > 0.0F);

    // Muting port A's channel 0 must not touch port B's, which is the regression.
    mask.reset();
    const float other_port_muted = render_port_b([&] { mask.set_muted(0, true); });
    CHECK(other_port_muted == unmuted);

    // And the part that is actually sounding must still be mutable.
    mask.reset();
    const float this_part_muted = render_port_b([&] { mask.set_muted(channels, true); });
    CHECK(this_part_muted == 0.0F);

    // Solo is the same question from the other side: soloing a part on port A leaves port B out.
    mask.reset();
    const float other_port_soloed = render_port_b([&] { mask.set_soloed(0, true); });
    CHECK(other_port_soloed == 0.0F);

    mask.reset();
    const float this_part_soloed = render_port_b([&] { mask.set_soloed(channels, true); });
    CHECK(this_part_soloed == unmuted);
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

namespace {

/// One track: any number of (meta type, name) tags, then a program change carrying `program`.
[[nodiscard]] std::vector<std::uint8_t>
named_track(std::initializer_list<std::pair<std::uint8_t, const char*>> tags,
            std::uint8_t program,
            int channel = 0)
{
    std::vector<std::uint8_t> data;
    for (const auto& [meta, name] : tags) {
        data.insert(data.end(), {0x00, 0xFF, meta, static_cast<std::uint8_t>(std::strlen(name))});
        data.insert(data.end(), name, name + std::strlen(name));
    }
    data.insert(data.end(),
                {0x00, static_cast<std::uint8_t>(0xC0 | channel), program, 0x00, 0xFF, 0x2F, 0x00});
    const auto length = static_cast<std::uint32_t>(data.size());
    std::vector<std::uint8_t> out{'M', 'T', 'r', 'k',
                                  static_cast<std::uint8_t>(length >> 24),
                                  static_cast<std::uint8_t>(length >> 16),
                                  static_cast<std::uint8_t>(length >> 8),
                                  static_cast<std::uint8_t>(length)};
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

/// A format-1 file from whole tracks.
[[nodiscard]] std::vector<std::uint8_t>
file_of(std::initializer_list<std::vector<std::uint8_t>> tracks)
{
    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 0, 0x01, 0xE0};
    file[11] = static_cast<std::uint8_t>(tracks.size());
    for (const auto& bytes : tracks) {
        file.insert(file.end(), bytes.begin(), bytes.end());
    }
    return file;
}

} // namespace

TEST_CASE("device names are assigned ports in order of first appearance", "[port][smf]")
{
    // A file with no FF 21 at all can still be multi-port: it names its outputs instead, with
    // FF 09 (Device Name) or -- in files older than that meta -- FF 04 (Instrument Name). The
    // names are opaque ("Modem" and "Printer" in the files this covers, the Mac serial ports), so
    // there is nothing to recognise; the reader stores each string as it occurs, deduplicated,
    // and the first distinct name is port 0, the second port 1, and so on.
    const std::uint8_t meta = GENERATE(std::uint8_t{0x09}, std::uint8_t{0x04});
    INFO("meta 0x" << std::hex << int(meta));

    const std::vector<MidiEvent> events =
        smf::parse(file_of({named_track({{meta, "Modem"}}, 40),    // first name seen: port 0
                            named_track({{meta, "Printer"}}, 41),  // second: port 1
                            named_track({{meta, "Modem"}}, 42)}),  // seen before: port 0 again
                   32000);

    REQUIRE(events.size() == 3);
    CHECK(events[0].port == 0);
    CHECK(events[1].port == 1);
    CHECK(events[2].port == 0);
}

TEST_CASE("names without a channel collision are labels, not ports", "[port][smf]")
{
    // Names only mean ports when the same MIDI channel is claimed under two different names --
    // overlapping channels are the one thing a name scheme exists to disambiguate. Measured over
    // a 128,000-file archive, 2,158 files carry two or more distinct FF 04 strings with no FF 21
    // or FF 09, and nearly all are ordinary single-port files with one instrument label per
    // track ("Flute", "CHA 1", "Hard Lead 5"); scattering those across ports rewrote half their
    // mix. The gate is judged per event's own channel, not per track, so a track that plays
    // several channels is counted where its events land.
    const std::uint8_t meta = GENERATE(std::uint8_t{0x09}, std::uint8_t{0x04});
    INFO("meta 0x" << std::hex << int(meta));

    SECTION("distinct labels on distinct channels stay on port zero")
    {
        // "Piano" even repeats across two channels; reuse alone is still not a collision.
        const std::vector<MidiEvent> events =
            smf::parse(file_of({named_track({{meta, "Piano"}}, 40, 0),
                                named_track({{meta, "Piano"}}, 41, 1),
                                named_track({{meta, "Strings"}}, 42, 2),
                                named_track({{meta, "Flute"}}, 43, 3)}),
                       32000);

        REQUIRE(events.size() == 4);
        for (const MidiEvent& event : events) {
            CHECK(event.port == 0);
        }
    }

    SECTION("a channel claimed by two names opens the gate for the whole file")
    {
        // Channel 0 under both names is what says these are devices; the channel-3 track rides
        // along on its own name's port, as it would on the hardware the file was written for.
        const std::vector<MidiEvent> events =
            smf::parse(file_of({named_track({{meta, "Modem"}}, 40, 0),
                                named_track({{meta, "Printer"}}, 41, 0),
                                named_track({{meta, "Printer"}}, 42, 3)}),
                       32000);

        REQUIRE(events.size() == 3);
        CHECK(events[0].port == 0);
        CHECK(events[1].port == 1);
        CHECK(events[2].port == 1);
    }

    SECTION("a track playing several channels gets no vote")
    {
        // The innerlight.mid shape: label tracks on their own channels, plus one named track
        // playing everything -- a mix-down riding along in the file. Its channels overlap every
        // label track's, and if it could vote, every file of this shape would read as
        // multi-port. A device voice, the only thing worth naming an output for, plays one
        // channel; a track playing several is not one.
        std::vector<std::uint8_t> mixdown{0x00, 0xFF, meta, 8,
                                          'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l',
                                          0x00, 0xC0, 40,
                                          0x00, 0xC1, 41,
                                          0x00, 0xC2, 42,
                                          0x00, 0xFF, 0x2F, 0x00};
        const auto length = static_cast<std::uint32_t>(mixdown.size());
        std::vector<std::uint8_t> track{'M', 'T', 'r', 'k',
                                        static_cast<std::uint8_t>(length >> 24),
                                        static_cast<std::uint8_t>(length >> 16),
                                        static_cast<std::uint8_t>(length >> 8),
                                        static_cast<std::uint8_t>(length)};
        track.insert(track.end(), mixdown.begin(), mixdown.end());

        const std::vector<MidiEvent> events =
            smf::parse(file_of({named_track({{meta, "CHA 1"}}, 50, 0),
                                named_track({{meta, "CHA 2"}}, 51, 1),
                                std::move(track)}),
                       32000);

        REQUIRE(events.size() == 5);
        for (const MidiEvent& event : events) {
            CHECK(event.port == 0);
        }
    }

    SECTION("two names from one single-channel track still collide")
    {
        // A prefix switch mid-track is a device switch on that channel, and it needs the ports
        // just as much as two tracks do.
        std::vector<std::uint8_t> switching{0x00, 0xFF, meta, 1, 'A',
                                            0x00, 0xC0, 40,
                                            0x00, 0xFF, meta, 1, 'B',
                                            0x00, 0xC0, 41,
                                            0x00, 0xFF, 0x2F, 0x00};
        const auto length = static_cast<std::uint32_t>(switching.size());
        std::vector<std::uint8_t> track{'M', 'T', 'r', 'k',
                                        static_cast<std::uint8_t>(length >> 24),
                                        static_cast<std::uint8_t>(length >> 16),
                                        static_cast<std::uint8_t>(length >> 8),
                                        static_cast<std::uint8_t>(length)};
        track.insert(track.end(), switching.begin(), switching.end());

        const std::vector<MidiEvent> events = smf::parse(file_of({std::move(track)}), 32000);

        REQUIRE(events.size() == 2);
        CHECK(events[0].port == 0); // "A"
        CHECK(events[1].port == 1); // "B", second device on the same channel
    }
}

TEST_CASE("one port scheme rules a file: FF 21, else FF 09, else FF 04", "[port][smf]")
{
    // The schemes do not mix. FF 21 carries actual numbers, so its presence anywhere in the file
    // wins outright; FF 09 is the meta defined for naming outputs, so it outranks FF 04, which is
    // only a device name by the convention of files older than FF 09 -- in anything newer it is
    // an actual instrument name, and letting it assign ports would scatter a single-port file.

    SECTION("FF 09 anywhere means FF 04 names are ignored")
    {
        const std::vector<MidiEvent> events =
            smf::parse(file_of({named_track({{0x04, "Alpha"}}, 40),
                                named_track({{0x04, "Beta"}, {0x09, "OutA"}}, 41),
                                named_track({{0x09, "OutB"}}, 42)}),
                       32000);

        REQUIRE(events.size() == 3);
        // Under FF 04 the file would be Alpha 0, Beta 1; under FF 09 the first track is untagged.
        CHECK(events[0].port == 0);
        CHECK(events[1].port == 0); // OutA, the first FF 09 name
        CHECK(events[2].port == 1); // OutB, the second
    }

    SECTION("FF 21 anywhere means every name is ignored")
    {
        auto tagged = named_track({{0x09, "OutA"}, {0x04, "Alpha"}}, 40);
        // Splice an FF 21 for port 2 in front of the name metas of the first track.
        const std::array<std::uint8_t, 5> port_meta{0x00, 0xFF, 0x21, 0x01, 0x02};
        tagged.insert(tagged.begin() + 8, port_meta.begin(), port_meta.end());
        tagged[7] += port_meta.size();

        const std::vector<MidiEvent> events =
            smf::parse(file_of({std::move(tagged),
                                named_track({{0x09, "OutB"}, {0x04, "Beta"}}, 41)}),
                       32000);

        REQUIRE(events.size() == 2);
        CHECK(events[0].port == 2); // the FF 21 number, not a name assignment
        CHECK(events[1].port == 0); // no FF 21 in this track, and names do not apply
    }
}

TEST_CASE("lont32.mid splits onto two ports by instrument-name metas", "[port][smf][sccore]")
{
    // A real two-port file with not one FF 21 or FF 09 in it: every track opens with FF 04 naming
    // "Modem" or "Printer". Thirty-two channels only exist here if those names become ports.
    const std::filesystem::path path = testdata::repository_root() / "testdata" / "lont32.mid";
    if (!std::filesystem::exists(path)) {
        SKIP("lont32.mid is not in testdata");
    }

    const std::vector<MidiEvent> events = smf::read(path, 32000);
    REQUIRE_FALSE(events.empty());

    std::array<int, 2> notes_per_port{};
    for (const MidiEvent& event : events) {
        CHECK(event.port >= 0);
        CHECK(event.port < 2);
        if (event.kind == MidiEventKind::channel && event.message_type() == 0x90
            && event.data2 > 0) {
            ++notes_per_port[static_cast<std::size_t>(event.port & 1)];
        }
    }

    // "Modem" is declared first, so it is port 0; the split is real, with notes on both.
    CHECK(notes_per_port[0] > 0);
    CHECK(notes_per_port[1] > 0);
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

namespace {

/// A one-track file built from raw event bytes, for the loop and EMIDI scanners.
[[nodiscard]] std::vector<std::uint8_t> smf_file(
    const std::vector<std::vector<std::uint8_t>>& tracks)
{
    std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 0, 0x01, 0xE0};
    file[10] = static_cast<std::uint8_t>(tracks.size() >> 8);
    file[11] = static_cast<std::uint8_t>(tracks.size());
    for (const std::vector<std::uint8_t>& body : tracks) {
        std::vector<std::uint8_t> data = body;
        data.insert(data.end(), {0x00, 0xFF, 0x2F, 0x00});
        const auto length = static_cast<std::uint32_t>(data.size());
        file.insert(file.end(), {'M', 'T', 'r', 'k'});
        file.insert(file.end(), {static_cast<std::uint8_t>(length >> 24),
                                 static_cast<std::uint8_t>(length >> 16),
                                 static_cast<std::uint8_t>(length >> 8),
                                 static_cast<std::uint8_t>(length)});
        file.insert(file.end(), data.begin(), data.end());
    }
    return file;
}

/// A controller at a tick, with the tick as a one-byte delta from the previous event.
[[nodiscard]] std::vector<std::uint8_t> cc(int delta, int controller, int value)
{
    return {static_cast<std::uint8_t>(delta), 0xB0, static_cast<std::uint8_t>(controller),
            static_cast<std::uint8_t>(value)};
}

[[nodiscard]] std::vector<std::uint8_t> note(int delta, int key)
{
    return {static_cast<std::uint8_t>(delta), 0x90, static_cast<std::uint8_t>(key), 100};
}

void append(std::vector<std::uint8_t>& into, const std::vector<std::uint8_t>& what)
{
    into.insert(into.end(), what.begin(), what.end());
}

} // namespace

TEST_CASE("CC 110 is a LeapFrog loop begin inside the song and a designation at its head", "[smf]")
{
    // Three conventions write CC 110 and CC 111 and they collide. Position is what separates the
    // two that survive a file with no CC 112-119: a track designation declares what a track *is*,
    // so it sits at the head before any content, while a LeapFrog loop begins inside the song.
    // Upstream measured the split as total -- 32 EMIDI files with every designation at tick 0 or 1,
    // four LeapFrog files whose lone CC 110 sits at tick 189 or later.
    SECTION("a CC 110 inside the song opens a loop that CC 111 closes")
    {
        std::vector<std::uint8_t> track;
        append(track, note(0, 60));
        append(track, cc(100, 110, 0)); // LeapFrog begin, well inside the song
        append(track, note(10, 62));
        append(track, cc(100, 111, 64)); // end, and its value is not zero
        const smf::Song song = smf::load(smf_file({track}), 32000, "leapfrog.mid");
        REQUIRE(song.loop);
        CHECK(song.loop->start > 0);
        CHECK(song.loop->end > song.loop->start);
        // An explicit end marker makes the loop soft, as the XMI and Touhou ends do.
        CHECK(song.loop->soft);
    }

    SECTION("a CC 110 at the head of the track is a designation and opens nothing")
    {
        // And it suppresses the RPG Maker reading of CC 111 as well: the file has claimed the pair
        // for a different convention, so a CC 111 in it is not a loop start.
        std::vector<std::uint8_t> track;
        append(track, cc(0, 110, 0)); // designation, tick 0
        append(track, note(0, 60));
        append(track, cc(100, 111, 0)); // would be an RPG Maker start on its own
        const smf::Song song = smf::load(smf_file({track}), 32000, "emidi.mid");
        CHECK_FALSE(song.loop);
    }

    SECTION("with no CC 110 at all, CC 111 value zero is an RPG Maker loop start")
    {
        std::vector<std::uint8_t> track;
        append(track, note(0, 60));
        append(track, cc(100, 111, 0));
        append(track, note(100, 62));
        const smf::Song song = smf::load(smf_file({track}), 32000, "rpgmaker.mid");
        REQUIRE(song.loop);
        CHECK(song.loop->start > 0);
        // No end marker, so the loop runs to the last voice event and stays hard.
        CHECK_FALSE(song.loop->soft);
    }

    SECTION("a CC 112-119 anywhere settles the file as EMIDI and voids both readings")
    {
        // Nothing but EMIDI touches 112-119, which is what makes it the test -- and why CC 110 and
        // CC 111 must not be allowed to mark a file as EMIDI themselves.
        std::vector<std::uint8_t> track;
        append(track, note(0, 60));
        append(track, cc(50, 116, 0)); // an EMIDI/XMI loop start, which is a real loop signal
        append(track, cc(50, 110, 0)); // inside the song, but this file is EMIDI
        append(track, cc(50, 111, 0));
        const smf::Song song = smf::load(smf_file({track}), 32000, "emidi-cc116.mid");
        REQUIRE(song.loop);
        // The XMI start, not the CC 110 that follows it.
        CHECK(song.loop->start < song.loop->end);
    }
}

TEST_CASE("EMIDI keeps the copy authored for this engine", "[smf]")
{
    // A song built for several cards duplicates its content, one copy per card, each marked with
    // CC 110 designations. Playing every copy doubles the voices. AudioLib numbers 0 General MIDI,
    // 1 Roland Sound Canvas, 127 every card.
    const auto designated = [](int device, int key) {
        std::vector<std::uint8_t> track;
        append(track, cc(0, 110, device));
        append(track, note(0, key));
        return track;
    };

    SECTION("the Sound Canvas copy wins when the file offers one, because that is what this is")
    {
        const smf::Song song =
            smf::load(smf_file({designated(0, 60), designated(1, 62)}), 32000, "both.mid");
        std::vector<int> keys;
        for (const MidiEvent& event : song.events) {
            if (event.message_type() == 0x90) {
                keys.push_back(event.data1);
            }
        }
        CHECK(keys == std::vector<int>{62});
    }

    SECTION("a file that only ever addresses General MIDI is not silenced")
    {
        // The hazard of picking a card and holding to it: every designated track would be dropped
        // and the file would play as nothing.
        const smf::Song song =
            smf::load(smf_file({designated(0, 60), designated(0, 62)}), 32000, "gm-only.mid");
        int notes = 0;
        for (const MidiEvent& event : song.events) {
            notes += event.message_type() == 0x90 ? 1 : 0;
        }
        CHECK(notes == 2);
    }

    SECTION("the wildcard and an undesignated track always play")
    {
        std::vector<std::uint8_t> plain;
        append(plain, note(0, 64));
        const smf::Song song = smf::load(
            smf_file({designated(1, 60), designated(127, 62), plain}), 32000, "wildcard.mid");
        int notes = 0;
        for (const MidiEvent& event : song.events) {
            notes += event.message_type() == 0x90 ? 1 : 0;
        }
        CHECK(notes == 3);
    }

    SECTION("one matching designation carries the track, however many others it lists")
    {
        // AudioLib latches its include flag on the first match and never clears it, so a track
        // naming several cards including ours sounds. Requiring all of them drops parts.
        std::vector<std::uint8_t> track;
        append(track, cc(0, 110, 4)); // Sound Blaster
        append(track, cc(0, 110, 1)); // and the Sound Canvas
        append(track, note(0, 60));
        const smf::Song song = smf::load(smf_file({track}), 32000, "multi.mid");
        int notes = 0;
        for (const MidiEvent& event : song.events) {
            notes += event.message_type() == 0x90 ? 1 : 0;
        }
        CHECK(notes == 1);
    }
}

TEST_CASE("the first note is found across tracks, not in the first track that has one", "[smf]")
{
    // A file whose first track rests until the second section would otherwise report that entry as
    // the song's start, which for a looping arrangement means skipping the whole introduction.
    std::vector<std::uint8_t> late;
    append(late, note(200, 72));
    std::vector<std::uint8_t> early;
    append(early, note(20, 60));

    const smf::Song song = smf::load(smf_file({late, early}), 32000, "two-tracks.mid");
    REQUIRE(song.first_note > 0);

    const smf::Song alone = smf::load(smf_file({early}), 32000, "one-track.mid");
    CHECK(song.first_note == alone.first_note);

    // A song that starts on a note has no lead-in to skip.
    std::vector<std::uint8_t> immediate;
    append(immediate, note(0, 60));
    CHECK(smf::load(smf_file({immediate}), 32000, "immediate.mid").first_note == 0);
}
