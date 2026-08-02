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
