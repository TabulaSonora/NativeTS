#include "dsp/wave_codec.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using namespace ts;

TEST_CASE("the scale nibble is selected per 16-sample block", "[codec]")
{
    // One byte covers 32 samples: the low nibble serves the first 16, the high nibble the second.
    // Getting this backwards scales every other block wrong, which is a buzzing distortion rather
    // than silence -- it sounds like a bad sample, not like a bug.
    constexpr std::array<std::uint8_t, 2> scale{0x5A, 0xC3};

    CHECK(codec::scale_at(scale, 0) == 0x0A);
    CHECK(codec::scale_at(scale, 15) == 0x0A);
    CHECK(codec::scale_at(scale, 16) == 0x05);
    CHECK(codec::scale_at(scale, 31) == 0x05);

    CHECK(codec::scale_at(scale, 32) == 0x03);
    CHECK(codec::scale_at(scale, 47) == 0x03);
    CHECK(codec::scale_at(scale, 48) == 0x0C);
    CHECK(codec::scale_at(scale, 63) == 0x0C);
}

TEST_CASE("the delta byte is read signed", "[codec]")
{
    // Reading it unsigned mirrors every downward slope, which is the difference between a waveform
    // and its rectified caricature.
    CHECK(codec::step(0x01, 0) == 1 << 10);
    CHECK(codec::step(0x7F, 0) == 127 << 10);
    CHECK(codec::step(0xFF, 0) == -(1 << 10));
    CHECK(codec::step(0x80, 0) == -128 * (1 << 10));
    CHECK(codec::step(0x00, 15) == 0);
}

TEST_CASE("the shift reaches 25 without invoking undefined behaviour", "[codec]")
{
    // scale is a 4-bit nibble and the bias is 10, so the count runs to 25 and the shifted value is
    // a negative signed byte. This is the single most dangerous expression in the port: in C it is
    // undefined, and the wrapping helper is what makes it defined and correct.
    CHECK(codec::step(0xFF, 15) == -33554432);
    CHECK(codec::step(0x01, 15) == 33554432);

    // 64 << 25 is exactly 2^31, one past INT32_MAX, so it lands on INT32_MIN.
    CHECK(codec::step(0x40, 15) == std::numeric_limits<std::int32_t>::min());

    // -128 << 25 is -2^32, which is exactly zero modulo 2^32. Every bit shifts out. This is the
    // sharpest case in the codec: on a saturating or a wider accumulator it would be a large
    // negative step instead, and the sample would be audibly wrong rather than silent.
    CHECK(codec::step(0x80, 15) == 0);
}

TEST_CASE("the predictor is a wrapping 32-bit accumulator", "[codec]")
{
    // Matching the engine's own field width. Traces compare these values directly against the
    // DLL's predictor, so a 64-bit accumulator "for safety" would diverge from the hardware.
    //
    // Delta 1 at exponent 15 is a step of exactly 2^25, so the accumulator lands on 2^31 after
    // precisely 64 of them -- one past INT32_MAX. That makes the wrap exact and checkable rather
    // than merely "it went negative somewhere".
    std::vector<std::uint8_t> delta(64, 0x01);
    std::vector<std::uint8_t> scale(8, 0xFF);

    std::vector<std::int32_t> predictors(delta.size());
    codec::decode_predictors(delta, scale, predictors);

    CHECK(predictors[0] == 33'554'432);
    CHECK(predictors[62] == 2'113'929'216);

    // The 64th step overshoots INT32_MAX and the accumulator wraps to INT32_MIN rather than
    // saturating. A saturating predictor would flatten the top of the wave instead.
    CHECK(predictors[63] == std::numeric_limits<std::int32_t>::min());
}

TEST_CASE("decoding integrates in integers and scales once", "[codec]")
{
    // Scaling per step and summing in float would accumulate rounding across a hundred thousand
    // samples; the engine integrates exactly and narrows at the end.
    constexpr std::array<std::uint8_t, 4> delta{0x01, 0x01, 0xFF, 0x00};
    constexpr std::array<std::uint8_t, 1> scale{0x00}; // exponent 0 -> step is delta << 10

    std::array<float, 4> samples{};
    codec::decode(delta, scale, samples);

    CHECK(samples[0] == static_cast<float>(1024 * codec::output_scale));
    CHECK(samples[1] == static_cast<float>(2048 * codec::output_scale));
    CHECK(samples[2] == static_cast<float>(1024 * codec::output_scale));
    CHECK(samples[3] == static_cast<float>(1024 * codec::output_scale));
}

TEST_CASE("the output scale is exactly two to the minus twenty-seven", "[codec]")
{
    // Written as a literal rather than computed, so this pins the literal.
    CHECK(codec::output_scale == 1.0 / 134217728.0);
    CHECK(codec::shift_bias == 10);
    CHECK(codec::samples_per_scale_nibble == 16);
    CHECK(codec::samples_per_scale_byte == 32);
}
