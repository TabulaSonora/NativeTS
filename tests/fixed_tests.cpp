#include "dsp/fixed.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

using namespace ts::fx;

// These assert the arithmetic the engine's control path is built on. Each case is a behaviour the
// upstream C# build gets from running with overflow checking disabled, and getting any of them
// wrong is silent: the render still produces audio, it is just the wrong audio.

TEST_CASE("wrapping multiply keeps the low 32 bits", "[fixed]")
{
    // The pitch-envelope depth scale reaches about 2.7e11 and the engine keeps only the low word.
    // 0xFFFF * 0xFFFF = 0xFFFE0001, which is negative as a signed 32-bit value.
    CHECK(wmul(0xFFFF, 0xFFFF) == static_cast<std::int32_t>(0xFFFE0001));

    CHECK(wmul(0x10000, 0x10000) == 0);
    CHECK(wmul(2, 3) == 6);
    CHECK(wmul(-2, 3) == -6);

    // Half-damper release scaling: (0xFFFF - (damper << 9)) * releaseRate overflows for a large
    // release rate. 33279 * 65535 = 2,180,939,265, past INT32_MAX (2,147,483,647).
    CHECK(wmul(33279, 65535) == static_cast<std::int32_t>(2180939265u));
}

TEST_CASE("wrapping add and subtract wrap rather than trapping", "[fixed]")
{
    constexpr auto max = std::numeric_limits<std::int32_t>::max();
    constexpr auto min = std::numeric_limits<std::int32_t>::min();

    CHECK(wadd(max, 1) == min);
    CHECK(wsub(min, 1) == max);
    CHECK(wadd(7, 5) == 12);
    CHECK(wsub(5, 7) == -2);
}

TEST_CASE("wrapping left shift matches the codec's predictor step", "[fixed]")
{
    // The codec computes (sbyte)delta << (scale + 10), where the shift count reaches 25 and the
    // shifted value is a signed delta byte. The predictor is a 32-bit accumulator that wraps.
    CHECK(wshl(-1, 31) == std::numeric_limits<std::int32_t>::min());
    CHECK(wshl(1, 31) == std::numeric_limits<std::int32_t>::min());
    CHECK(wshl(-128, 25) == 0);
    CHECK(wshl(-1, 25) == -33554432);
    CHECK(wshl(3, 4) == 48);
}

TEST_CASE("width truncations match the C# casts", "[fixed]")
{
    CHECK(i16(0x1FFFF) == -1);
    CHECK(i16(0x8000) == std::numeric_limits<std::int16_t>::min());
    CHECK(i16(0x7FFF) == 32767);

    // The TVA velocity crossfade genuinely wraps here: curve(255) * |span|(127) doubled and
    // shifted reaches 254, which as a signed byte is -2.
    CHECK(i8(254) == -2);
    CHECK(i8(0x1FF) == -1);
    CHECK(i8(127) == 127);

    CHECK(u16(-1) == 0xFFFF);
    CHECK(u8(0x1FF) == 0xFF);
}

TEST_CASE("shift8 truncates between the shifts", "[fixed]")
{
    // (short)(value << 2) >> 8. The 16-bit truncation in the middle is the point: it makes a large
    // product wrap rather than saturate, and the result stays signed so the shift is arithmetic.
    CHECK(shift8(0) == 0);
    CHECK(shift8(64) == 1);
    CHECK(shift8(-64) == -1);

    // Without the truncation this would be 0x4000; with it the value wraps negative.
    CHECK(shift8(0x4000) == 0);
    CHECK(shift8(0x2000) == -128);

    // The negative branch has to index the low end of the output curve, which only holds if the
    // right shift is arithmetic. On an unsigned type this would come out enormous and positive.
    CHECK(shift8(-1) < 0);
}

TEST_CASE("little-endian field reads are host independent", "[fixed]")
{
    constexpr std::uint8_t data[] = {0x34, 0x12, 0xFF, 0xFF, 0x78, 0x56, 0x34, 0x12};

    CHECK(read_u16le(data) == 0x1234);
    CHECK(read_i16le(data) == 0x1234);
    CHECK(read_i16le(data + 2) == -1);
    CHECK(read_u16le(data + 2) == 0xFFFF);
    CHECK(read_u32le(data + 4) == 0x12345678u);
}
