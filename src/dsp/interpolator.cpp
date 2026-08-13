#include "tabulasonora/interpolator.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ts {

float Interpolator::sample(std::span<const float> buffer, double position) const noexcept
{
    // std::floor, not a cast: a cast truncates toward zero, which for a negative position would
    // round the wrong way and pick up the phase from the far side of the sample.
    auto index = static_cast<int>(std::floor(position));
    const int phase =
        std::clamp(static_cast<int>((position - index) * phase_count), 0, phase_count - 1);
    index = std::clamp(index, 1, static_cast<int>(buffer.size()) - 3);

    const auto c = static_cast<std::size_t>(phase * tap_count);
    const auto i = static_cast<std::size_t>(index);

    // Left to right, in float, with no reassociation and no FMA contraction. The build enforces
    // the latter; this expression's shape is the former.
    return (coefficients_[c] * buffer[i - 1]) + (coefficients_[c + 1] * buffer[i])
           + (coefficients_[c + 2] * buffer[i + 1]) + (coefficients_[c + 3] * buffer[i + 2]);
}

float Interpolator::sample_ring(std::span<const float> ring,
                                std::int64_t index,
                                double fraction) const noexcept
{
    const auto mask = static_cast<std::int64_t>(ring.size()) - 1;
    const int phase = std::clamp(static_cast<int>(fraction * phase_count), 0, phase_count - 1);
    const auto c = static_cast<std::size_t>(phase * tap_count);

    const auto at = [ring, mask](std::int64_t i) noexcept {
        return ring[static_cast<std::size_t>(i & mask)];
    };

    return (coefficients_[c] * at(index - 1)) + (coefficients_[c + 1] * at(index))
           + (coefficients_[c + 2] * at(index + 1)) + (coefficients_[c + 3] * at(index + 2));
}

void Interpolator::resample(std::span<const float> buffer,
                            std::span<const double> positions,
                            std::span<float> destination) const noexcept
{
    const std::size_t count = std::min(destination.size(), positions.size());
    for (std::size_t i = 0; i < count; ++i) {
        destination[i] = sample(buffer, positions[i]);
    }
}

SincInterpolator::SincInterpolator()
{
    // Tabulated rather than evaluated: the inner loop asks for up to 64 weights per output sample,
    // and a sine and a pair of cosines apiece would dominate the render. One entry per 1/1024 of a
    // source sample, read with linear interpolation, puts the table error far below the fit error.
    //
    // Only `|d|` is stored, because both factors are even in `d` -- the sinc is odd over odd,
    // and the window's cosines are invariant under the `t -> 1-t` that `d -> -d` induces. The
    // last entry holds `kernel(radius) = 0` and exists so `weight` may still read `i + 1` from
    // the final real entry.
    const auto count = static_cast<std::size_t>((radius * resolution) + 1);
    table_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double d = static_cast<double>(i) / resolution;
        if (d >= radius) {
            table_[i] = 0.0;
            continue;
        }
        const double x = 2.0 * cutoff * d;
        const double sinc = (x == 0.0) ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        // A Blackman with the shoulder left free, which is the second fitted constant.
        const double t = ((d / radius) + 1.0) / 2.0;
        const double window = ((1.0 - shoulder) / 2.0) - (0.5 * std::cos(2.0 * std::numbers::pi * t))
                              + ((shoulder / 2.0) * std::cos(4.0 * std::numbers::pi * t));
        table_[i] = sinc * window;
    }
}

const SincInterpolator& SincInterpolator::shared() noexcept
{
    static const SincInterpolator instance;
    return instance;
}

double SincInterpolator::weight(double d) const noexcept
{
    const double at = std::abs(d) * resolution;
    if (at >= static_cast<double>(table_.size() - 1)) {
        return 0.0;
    }
    const auto i = static_cast<std::size_t>(at);
    const double f = at - static_cast<double>(i);
    return (table_[i] * (1.0 - f)) + (table_[i + 1] * f);
}

namespace {

/// The scale the kernel argument takes, and the half-width that scale then needs.
///
/// `scale` is `min(1, 1/ratio)` -- reading faster stretches the kernel over more source samples and
/// drops its cutoff to the output rate, which is the band-limiting. The half-width has to follow it
/// or the taps the stretch calls for are simply not summed; capped, because the cost is linear and
/// a runaway ratio must not be able to ask for an unbounded loop.
struct Span {
    double scale;
    int half;
};

[[nodiscard]] Span span_for(double ratio) noexcept
{
    const double scale = (ratio > 1.0) ? (1.0 / ratio) : 1.0;
    const auto half = static_cast<int>(
        std::min<double>(SincInterpolator::max_radius, std::ceil(SincInterpolator::radius / scale)));
    return {scale, std::max(SincInterpolator::radius, half)};
}

}   // namespace

float SincInterpolator::sample(std::span<const float> buffer,
                               double position,
                               double ratio) const noexcept
{
    if (buffer.empty()) {
        return 0.0F;
    }
    const auto base = static_cast<std::int64_t>(std::floor(position));
    const double fraction = position - static_cast<double>(base);
    const auto [scale, half] = span_for(ratio);

    const auto last = static_cast<std::int64_t>(buffer.size()) - 1;
    double sum = 0.0;
    double density = 0.0;
    for (int m = -half + 1; m <= half; ++m) {
        const double w = weight((static_cast<double>(m) - fraction) * scale);
        if (w == 0.0) {
            continue;
        }
        const std::int64_t at = std::clamp(base + m, std::int64_t{0}, last);
        sum += buffer[static_cast<std::size_t>(at)] * w;
        density += w;
    }
    // Normalised by the weights actually summed, so every fractional position has unity DC gain
    // even where the window is truncated at a buffer edge.
    return density > 0.0 ? static_cast<float>(sum / density) : 0.0F;
}

float SincInterpolator::sample_ring(std::span<const float> ring,
                                    std::int64_t index,
                                    double fraction,
                                    double ratio) const noexcept
{
    const auto mask = static_cast<std::int64_t>(ring.size()) - 1;
    const auto [scale, half] = span_for(ratio);

    double sum = 0.0;
    double density = 0.0;
    for (int m = -half + 1; m <= half; ++m) {
        const double w = weight((static_cast<double>(m) - fraction) * scale);
        if (w == 0.0) {
            continue;
        }
        sum += ring[static_cast<std::size_t>((index + m) & mask)] * w;
        density += w;
    }
    return density > 0.0 ? static_cast<float>(sum / density) : 0.0F;
}

std::pair<double, double> PanLaw::gains(int pan) const noexcept
{
    const int p = std::clamp(pan, 0, 127);
    const double left = table_[static_cast<std::size_t>(127 - p)] / 127.0;
    const double right = p == 0 ? 0.0 : table_[static_cast<std::size_t>(p - 1)] / 127.0;
    return {left, right};
}

} // namespace ts
