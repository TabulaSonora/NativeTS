#pragma once

#include <cstdint>

/// Wrapping fixed-point primitives.
///
/// The original engine's control path is exclusively 16-bit fixed point, and a number of its
/// expressions depend on *wrapping* or on truncation direction rather than merely tolerating them.
/// The upstream C# build disables overflow checking project-wide and records that this is
/// load-bearing, not an optimisation.
///
/// C++20 already defines most of what that needs: signed integers are two's complement, `>>` on a
/// signed value is an arithmetic shift, `<<` is congruent modulo 2^N, and a narrowing conversion to
/// a signed type is modular. What it still leaves undefined is signed overflow from `+`, `-` and
/// `*`.
///
/// So every expression that is *meant* to overflow goes through a helper here. The build also
/// passes `-fwrapv`, but that is belt-and-braces: routing the arithmetic through these functions is
/// what keeps the intent visible at the call site and the behaviour independent of the toolchain.
/// An ordinary `a * b` in this codebase should be read as a claim that the product fits.
///
/// One caveat, paid for. Being well-defined is not the same as being compiled correctly:
/// `i16(x) >= 0` as a **branch condition** was miscompiled by MSVC under optimisation, which tested
/// the wrong bit and made Windows renders differ from Linux ones. Every helper here is defined
/// behaviour and every toolchain agrees on what it means; that only guarantees the *intent* is
/// unambiguous. Where one of these feeds a comparison that decides a branch, prefer saying which
/// bit is being tested — and run the suite on both toolchains, because a single-platform run cannot
/// see this class of defect at all. \ref verification has the case.
namespace ts::fx {

/// Wrapping addition.
[[nodiscard]] constexpr std::int32_t wadd(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

/// Wrapping subtraction.
[[nodiscard]] constexpr std::int32_t wsub(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

/// Wrapping multiplication.
///
/// This is the workhorse. Several control-path products genuinely exceed 32 bits — a pitch-envelope
/// depth scale reaches about 2.7e11 — and the engine keeps only the low word. Widening one of these
/// to 64 bits "to be safe" changes the result and is a defect, not a fix.
[[nodiscard]] constexpr std::int32_t wmul(std::int32_t a, std::int32_t b) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

/// Wrapping left shift.
///
/// Used by the sample codec, where the shift count reaches 25 and the shifted value is a signed
/// delta byte. C++20 defines this for signed operands, but the helper keeps it uniform with the
/// rest and documents that the overflow is intended.
[[nodiscard]] constexpr std::int32_t wshl(std::int32_t value, int count) noexcept
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << count);
}

/// Truncates to 16 bits signed — the C# `(short)` cast.
[[nodiscard]] constexpr std::int16_t i16(std::int32_t value) noexcept
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}

/// Truncates to 8 bits signed — the C# `(sbyte)` cast.
[[nodiscard]] constexpr std::int8_t i8(std::int32_t value) noexcept
{
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(value));
}

/// Truncates to 16 bits unsigned — the C# `(ushort)` cast.
[[nodiscard]] constexpr std::uint16_t u16(std::int32_t value) noexcept
{
    return static_cast<std::uint16_t>(value);
}

/// Truncates to 8 bits unsigned — the C# `(byte)` cast.
[[nodiscard]] constexpr std::uint8_t u8(std::int32_t value) noexcept
{
    return static_cast<std::uint8_t>(value);
}

/// The engine's recurring "scale by 2 and rescale" step: `(short)(value << 2) >> 8`.
///
/// The 16-bit truncation *between* the two shifts is the point of it. It is what makes a large
/// product wrap rather than saturate, and the result stays a plain `int` so the right shift is
/// arithmetic — on an unsigned type it would be logical and the negative branch would index the
/// wrong end of the output curve.
[[nodiscard]] constexpr std::int32_t shift8(std::int32_t value) noexcept
{
    return i16(wshl(value, 2)) >> 8;
}

/// Reads a little-endian signed 16-bit field from a byte buffer.
///
/// Written as an explicit byte pair rather than a reinterpret so it is endian-neutral: the tables
/// are little-endian regardless of the host, and only the wholesale table casts need a
/// little-endian host at all.
[[nodiscard]] constexpr std::int16_t read_i16le(const std::uint8_t* data) noexcept
{
    return i16(static_cast<std::int32_t>(data[0]) | (static_cast<std::int32_t>(data[1]) << 8));
}

/// Reads a little-endian unsigned 16-bit field from a byte buffer.
[[nodiscard]] constexpr std::uint16_t read_u16le(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(data[0])
                                      | (static_cast<std::uint32_t>(data[1]) << 8));
}

/// Reads a little-endian unsigned 32-bit field from a byte buffer.
[[nodiscard]] constexpr std::uint32_t read_u32le(const std::uint8_t* data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8)
           | (static_cast<std::uint32_t>(data[2]) << 16)
           | (static_cast<std::uint32_t>(data[3]) << 24);
}

} // namespace ts::fx
