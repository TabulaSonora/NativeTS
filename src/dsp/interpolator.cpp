#include "tabulasonora/interpolator.hpp"

#include <algorithm>
#include <cmath>

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

std::pair<double, double> PanLaw::gains(int pan) const noexcept
{
    const int p = std::clamp(pan, 0, 127);
    const double left = table_[static_cast<std::size_t>(127 - p)] / 127.0;
    const double right = p == 0 ? 0.0 : table_[static_cast<std::size_t>(p - 1)] / 127.0;
    return {left, right};
}

} // namespace ts
