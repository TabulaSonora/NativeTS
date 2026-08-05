#include "render_metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace testmetrics {

void transform(std::vector<double>& real, std::vector<double>& imag)
{
    const std::size_t size = real.size();
    for (std::size_t i = 1, j = 0; i < size; ++i) {
        std::size_t bit = size >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j |= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }

    for (std::size_t length = 2; length <= size; length <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(length);
        const double wr = std::cos(angle);
        const double wi = std::sin(angle);
        for (std::size_t i = 0; i < size; i += length) {
            double cr = 1.0;
            double ci = 0.0;
            for (std::size_t k = 0; k < length / 2; ++k) {
                const double ur = real[i + k];
                const double ui = imag[i + k];
                const double vr = real[i + k + length / 2] * cr - imag[i + k + length / 2] * ci;
                const double vi = real[i + k + length / 2] * ci + imag[i + k + length / 2] * cr;
                real[i + k] = ur + vr;
                imag[i + k] = ui + vi;
                real[i + k + length / 2] = ur - vr;
                imag[i + k + length / 2] = ui - vi;
                const double next_r = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next_r;
            }
        }
    }
}

namespace {

/// The Hann-windowed power spectrum both measures below read, or empty when it does not fit.
///
/// The size is rounded *up*, exactly as the generators round it, because two sides that round
/// differently are two sides measuring different windows. That can overshoot the input for a render
/// shorter than 64 Ki frames, which is a case neither generator survives either -- Python raises on
/// the slice. Returning nothing lets the caller say so instead of reading past the end.
[[nodiscard]] std::vector<double> spectrum(const std::vector<double>& mono, double start_fraction)
{
    std::size_t size = 1;
    while (size < std::min<std::size_t>(mono.size(), 1U << 16U)) {
        size <<= 1;
    }
    if (size < 256 || size > mono.size()) {
        return {};
    }

    const auto wanted = static_cast<std::size_t>(static_cast<double>(mono.size()) * start_fraction);
    const std::size_t start = std::min(mono.size() - size, wanted);

    std::vector<double> real(size);
    std::vector<double> imag(size, 0.0);
    for (std::size_t i = 0; i < size; ++i) {
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i)
                                 / static_cast<double>(size));
        real[i] = mono[start + i] * window;
    }
    transform(real, imag);

    std::vector<double> power(size / 2 + 1);
    for (std::size_t k = 0; k < power.size(); ++k) {
        power[k] = real[k] * real[k] + imag[k] * imag[k];
    }
    return power;
}

} // namespace

std::vector<double> octave_bands(const std::vector<double>& mono,
                                 int rate,
                                 std::span<const int> centres,
                                 double start_fraction)
{
    const std::vector<double> power = spectrum(mono, start_fraction);
    if (power.empty()) {
        return {};
    }
    const std::size_t size = (power.size() - 1) * 2;

    std::vector<double> bands;
    for (const int centre : centres) {
        const double low = centre / std::numbers::sqrt2;
        const double high = centre * std::numbers::sqrt2;
        double total = 0.0;
        for (std::size_t k = 0; k < power.size(); ++k) {
            const double frequency = static_cast<double>(k) * rate / static_cast<double>(size);
            if (frequency >= low && frequency < high) {
                total += power[k];
            }
        }
        bands.push_back(total > 0.0 ? 10.0 * std::log10(total) : -999.0);
    }
    return bands;
}

double fundamental(const std::vector<double>& mono, int rate, double target, double start_fraction)
{
    const std::vector<double> power = spectrum(mono, start_fraction);
    if (power.empty()) {
        return 0.0;
    }
    const auto size = static_cast<double>((power.size() - 1) * 2);

    // A window of +-3%: wide enough for any patch's own detune, narrow enough that it cannot lock
    // onto a neighbouring harmonic.
    const auto low = std::max<std::size_t>(1, static_cast<std::size_t>(target * 0.97 * size / rate));
    const auto high = std::min(power.size() - 2, static_cast<std::size_t>(target * 1.03 * size / rate));
    if (high <= low) {
        return 0.0;
    }

    std::size_t peak = low;
    for (std::size_t k = low; k <= high; ++k) {
        if (power[k] > power[peak]) {
            peak = k;
        }
    }
    if (power[peak] <= 0.0) {
        return 0.0;
    }

    const double a = std::log(power[peak - 1] + 1e-30);
    const double b = std::log(power[peak] + 1e-30);
    const double c = std::log(power[peak + 1] + 1e-30);
    const double divisor = a - 2.0 * b + c;
    const double offset = divisor != 0.0 ? 0.5 * (a - c) / divisor : 0.0;
    return (static_cast<double>(peak) + offset) * rate / size;
}

std::vector<double> rms_envelope(const std::vector<double>& mono, int windows)
{
    std::vector<double> out;
    if (mono.empty()) {
        return out;
    }
    const std::size_t span =
        std::max<std::size_t>(1, mono.size() / static_cast<std::size_t>(windows));
    for (int w = 0; w < windows; ++w) {
        const std::size_t begin = static_cast<std::size_t>(w) * span;
        if (begin >= mono.size()) {
            break;
        }
        const std::size_t end = std::min(begin + span, mono.size());
        double energy = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            energy += mono[i] * mono[i];
        }
        energy /= static_cast<double>(end - begin);
        out.push_back(energy > 0.0 ? 10.0 * std::log10(energy) : -999.0);
    }
    return out;
}

std::vector<double> balance_envelope(const std::vector<double>& left,
                                     const std::vector<double>& right,
                                     int windows)
{
    std::vector<double> out;
    if (left.empty() || left.size() != right.size()) {
        return out;
    }
    const std::size_t span =
        std::max<std::size_t>(1, left.size() / static_cast<std::size_t>(windows));
    for (int w = 0; w < windows; ++w) {
        const std::size_t begin = static_cast<std::size_t>(w) * span;
        if (begin >= left.size()) {
            break;
        }
        const std::size_t end = std::min(begin + span, left.size());
        double left_energy = 0.0;
        double right_energy = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            left_energy += left[i] * left[i];
            right_energy += right[i] * right[i];
        }
        const auto count = static_cast<double>(end - begin);
        const double left_rms = std::sqrt(left_energy / count);
        const double right_rms = std::sqrt(right_energy / count);
        out.push_back(left_rms + right_rms > 0.0 ? right_rms / (left_rms + right_rms) : 0.5);
    }
    return out;
}

} // namespace testmetrics
