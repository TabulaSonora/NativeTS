#pragma once

#include "dsp/fixed.hpp"

#include <cstdint>
#include <span>

namespace ts::codec {

/// The wave ROM's block-floating-point DPCM codec.
///
/// Each sample contributes one signed delta byte; each 16-sample block contributes a 4-bit shift
/// exponent, packed two blocks to a byte. Decoding integrates the shifted deltas into a predictor
/// and scales the result by 2⁻²⁷:
///
///     predictor += (int8_t)delta[i] << (scale(i) + 10);
///     output[i]  = predictor * 2^-27;
///
/// The predictor is a pure integrator with no leak, which is what lets looping, ping-pong and
/// reverse playback all happen in the delta domain: rewinding the delta index while keeping the
/// predictor makes a loop seamless by construction. It is also why each loop pass adds a constant
/// DC step.

/// The codec's output scale, exactly 2⁻²⁷, which maps the integrated predictor into roughly [-1,
/// 1].
inline constexpr double output_scale = 7.450580596923828e-09;

/// Number of samples covered by one shift-exponent nibble.
inline constexpr int samples_per_scale_nibble = 16;

/// Number of samples covered by one shift-exponent byte.
inline constexpr int samples_per_scale_byte = 32;

/// Fixed bias added to every shift exponent.
inline constexpr int shift_bias = 10;

/// Reads the shift exponent that applies to one sample position.
///
/// One byte covers 32 samples: the low nibble serves the first 16, the high nibble the second 16.
[[nodiscard]] constexpr int scale_at(std::span<const std::uint8_t> scale, int position) noexcept
{
    const std::uint8_t packed = scale[static_cast<std::size_t>(position >> 5)];
    return ((position >> 4) & 1) == 0 ? packed & 0x0F : (packed >> 4) & 0x0F;
}

/// Computes the predictor increment for one sample.
///
/// The delta is signed and the shift count reaches 25, so this overflows by design; `wshl` is what
/// says so. Reading the delta as unsigned instead would mirror every downward slope.
[[nodiscard]] constexpr std::int32_t step(std::uint8_t delta, int scale) noexcept
{
    return fx::wshl(fx::i8(delta), scale + shift_bias);
}

/// Integrates the delta stream into the raw predictor values, before output scaling.
///
/// The predictor is a 32-bit accumulator, matching the engine's own field, and wraps rather than
/// saturating. Traces compare these values directly against the DLL's predictor, so the width
/// matters as much as the arithmetic.
/// `scale_phase` is where the wave's first sample sits inside its exponent block — `loop & 0x1F`.
/// It is not a correction for a malformed descriptor: two in five waves begin partway into a block,
/// which the codec permits because it stores differences and no absolute value per block. The
/// exponents therefore have to be indexed by absolute position, which is what the phase supplies.
constexpr void decode_predictors(std::span<const std::uint8_t> delta,
                                 std::span<const std::uint8_t> scale,
                                 std::span<std::int32_t> destination,
                                 int scale_phase = 0) noexcept
{
    std::int32_t predictor = 0;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        predictor =
            fx::wadd(predictor, step(delta[i], scale_at(scale, scale_phase + static_cast<int>(i))));
        destination[i] = predictor;
    }
}

/// Decodes the delta and scale streams into normalised samples.
///
/// The predictor is integrated in exact integer arithmetic and only then scaled, so no rounding
/// accumulates across the wave. The result is narrowed to `float` to match the engine's
/// stage-boundary precision -- the multiply happens in `double` and is narrowed once, which is not
/// the same as multiplying in `float`.
constexpr void decode(std::span<const std::uint8_t> delta,
                      std::span<const std::uint8_t> scale,
                      std::span<float> destination,
                      int scale_phase = 0) noexcept
{
    std::int32_t predictor = 0;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        predictor =
            fx::wadd(predictor, step(delta[i], scale_at(scale, scale_phase + static_cast<int>(i))));
        destination[i] = static_cast<float>(static_cast<double>(predictor) * output_scale);
    }
}

} // namespace ts::codec
