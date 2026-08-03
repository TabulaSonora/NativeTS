#include "tabulasonora/equalizer.hpp"

#include <cstddef>

namespace ts {

Equalizer::Equalizer(const EffectPresets& presets) noexcept
    : presets_(presets.has_eq() ? &presets : nullptr)
{
    refresh();
}

void Equalizer::set_low_frequency(int value) noexcept
{
    // `sysex_eq_params` tests `value < 2` and drops anything else on the floor rather than
    // clamping, so a stray byte leaves the setting where it was.
    if (value < 2) {
        low_frequency_ = value;
        refresh();
    }
}

void Equalizer::set_low_gain(int value) noexcept
{
    // The engine's own test is `(value - 0x34) < 0x19` in unsigned arithmetic, which accepts
    // 0x34-0x4C and rejects everything below as well as above.
    if (static_cast<unsigned>(value - EqPresets::gain_base) < EqPresets::gain_count) {
        low_gain_ = value;
        refresh();
    }
}

void Equalizer::set_high_frequency(int value) noexcept
{
    if (value < 2) {
        high_frequency_ = value;
        refresh();
    }
}

void Equalizer::set_high_gain(int value) noexcept
{
    if (static_cast<unsigned>(value - EqPresets::gain_base) < EqPresets::gain_count) {
        high_gain_ = value;
        refresh();
    }
}

void Equalizer::reset() noexcept
{
    low_frequency_ = 0;
    low_gain_ = flat_gain;
    high_frequency_ = 0;
    high_gain_ = flat_gain;
    clear();
    refresh();
}

void Equalizer::clear() noexcept
{
    low_left_ = State{};
    low_right_ = State{};
    high_left_ = State{};
    high_right_ = State{};
}

void Equalizer::refresh() noexcept
{
    if (presets_ == nullptr) {
        return;
    }
    low_ = presets_->eq().low_band(low_frequency_, low_gain_);
    high_ = presets_->eq().high_band(high_frequency_, high_gain_);
}

double Equalizer::step(const EqBand& band, State& state, double input) noexcept
{
    const double output = (band.b0 * input) + (band.b1 * state.x1) + (band.a1 * state.y1);
    state.x1 = input;
    state.y1 = output;
    return output;
}

void Equalizer::process(std::span<float> left, std::span<float> right) noexcept
{
    // Skipping the flat case is an optimisation that costs nothing in fidelity: at 0 dB the shelf's
    // numerator and denominator are the same polynomial, so it passes its input through exactly.
    // The filter memory is left alone rather than cleared -- a part switching the EQ back on should
    // not hear the tail of what it was doing before restart from silence.
    if (presets_ == nullptr || is_flat()) {
        return;
    }

    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t n = 0; n < count; ++n) {
        double l = left[n];
        double r = right[n];

        l = step(low_, low_left_, l);
        r = step(low_, low_right_, r);
        l = step(high_, high_left_, l);
        r = step(high_, high_right_, r);

        left[n] = static_cast<float>(l);
        right[n] = static_cast<float>(r);
    }
}

} // namespace ts
