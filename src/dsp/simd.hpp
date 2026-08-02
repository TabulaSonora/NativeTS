#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

/// The elementwise buffer kernels the mix paths repeat, written once.
///
/// **These are deliberately scalar.** The upstream C# vectorises them and its whole claim is that
/// each vector body is *bit-identical* to the scalar loop it replaces — the scalar loop being the
/// reference, not merely the tail handler. This port starts from that reference. Vectorising is a
/// later, optional step that must be asserted equal at every length either side of a vector
/// boundary, and until then there is nothing to diverge.
///
/// Two properties make these safe to vectorise later, and they constrain how they may be written
/// now. First, none is a horizontal reduction over floats: each output element is a function of the
/// input element at the same index, so no addition is ever reassociated. `peak_abs` is the one
/// reduction and is admissible only because maximum is exact under any association.
///
/// Second, the kernels that take a `double` gain keep the arithmetic in `double` and narrow once at
/// the store. Dropping to `float` would be faster and wrong.
namespace ts::simd {

/// Accumulates one buffer into another: `destination[i] += source[i]`.
inline void add(std::span<const float> source, std::span<float> destination) noexcept
{
    const std::size_t count = std::min(source.size(), destination.size());
    for (std::size_t i = 0; i < count; ++i) {
        destination[i] += source[i];
    }
}

/// Accumulates a scaled buffer: `destination[i] += (float)(source[i] * gain)`.
inline void
mix_scaled(std::span<const float> source, double gain, std::span<float> destination) noexcept
{
    const std::size_t count = std::min(source.size(), destination.size());
    for (std::size_t i = 0; i < count; ++i) {
        destination[i] += static_cast<float>(static_cast<double>(source[i]) * gain);
    }
}

/// Accumulates a twice-scaled buffer: `destination[i] += (float)((source[i] * first) * second)`.
///
/// The two multiplies stay separate on purpose. Floating-point multiplication is no more
/// associative than addition is, so `(x*a)*b` and `x*(a*b)` differ in the last place; collapsing
/// them would be the cheaper kernel and a silently different render.
inline void mix_scaled(std::span<const float> source,
                       double first,
                       double second,
                       std::span<float> destination) noexcept
{
    const std::size_t count = std::min(source.size(), destination.size());
    for (std::size_t i = 0; i < count; ++i) {
        destination[i] += static_cast<float>((static_cast<double>(source[i]) * first) * second);
    }
}

/// Stores a scaled buffer: `destination[i] = (float)(source[i] * gain)`.
inline void
store_scaled(std::span<const float> source, double gain, std::span<float> destination) noexcept
{
    const std::size_t count = std::min(source.size(), destination.size());
    for (std::size_t i = 0; i < count; ++i) {
        destination[i] = static_cast<float>(static_cast<double>(source[i]) * gain);
    }
}

/// Scales a buffer in place.
inline void scale(std::span<float> buffer, double gain) noexcept
{
    for (float& value : buffer) {
        value = static_cast<float>(static_cast<double>(value) * gain);
    }
}

/// Scales a buffer in place by a per-sample gain.
inline void scale_varying(std::span<float> buffer, std::span<const double> gains) noexcept
{
    const std::size_t count = std::min(buffer.size(), gains.size());
    for (std::size_t i = 0; i < count; ++i) {
        buffer[i] = static_cast<float>(static_cast<double>(buffer[i]) * gains[i]);
    }
}

/// The largest absolute value in a buffer.
///
/// The one reduction here, and admissible because maximum is exact under any association — there is
/// no rounding to reorder. Operating on absolute values also removes the signed-zero question. A
/// NaN is out of contract: it is already a bug upstream.
[[nodiscard]] inline float peak_abs(std::span<const float> buffer) noexcept
{
    float peak = 0.0F;
    for (float value : buffer) {
        peak = std::max(peak, std::abs(value));
    }
    return peak;
}

/// Whether any element is non-zero.
[[nodiscard]] inline bool any_non_zero(std::span<const float> buffer) noexcept
{
    return std::any_of(buffer.begin(), buffer.end(), [](float v) { return v != 0.0F; });
}

/// Whether any element is non-zero.
[[nodiscard]] inline bool any_non_zero(std::span<const double> buffer) noexcept
{
    return std::any_of(buffer.begin(), buffer.end(), [](double v) { return v != 0.0; });
}

} // namespace ts::simd
