#include "tabulasonora/lfo_engine.hpp"

#include "dsp/fixed.hpp"
#include "tabulasonora/tone.hpp"

#include <algorithm>
#include <cmath>

namespace ts {
namespace {

[[nodiscard]] int read_u16(std::span<const std::uint8_t> source, int offset) noexcept
{
    return source[static_cast<std::size_t>(offset)]
           | (source[static_cast<std::size_t>(offset) + 1] << 8);
}

[[nodiscard]] int read_i16(std::span<const std::uint8_t> source, int offset) noexcept
{
    return fx::read_i16le(source.data() + offset);
}

} // namespace

LfoEngine::LfoEngine(const TableSet& tables)
    : half_sine_(tables.lfo_wave()),
      rate_table_(tables.lfo_rate()),
      cents_table_(tables.lfo_cents()),
      delay_table_(tables.lfo_delay()),
      wave_map_(tables.lfo_wave_map()),
      wave_bank_(tables.lfo_wave_bank()),
      tone_(tables.tone())
{
}

int LfoEngine::waveform(int phase, int waveform_index) const noexcept
{
    const int p = phase & 0xFFFF;

    switch (waveform_index) {
    case 0: {
        // A half-sine table mirrored by sign for the second half.
        const int value = half_sine_[static_cast<std::size_t>((p >> 8) & 0x7F)];
        return p > 0x8000 ? value * -0x80 : value * 0x80;
    }

    case 4:
        return fx::i16(((((p >= 0x8000 ? p - 0x10000 : p) >> 15) & 2) + 0x7FFF));

    case 5:
        return fx::i16(p - 0x8000);

    case 6: {
        // The whole shape is 16-bit arithmetic: p * 2 overshoots and the truncation is what folds
        // each quadrant back into range.
        const int quadrant = p >> 14;
        switch (quadrant) {
        case 1:
            return fx::i16(~(p * 2));
        case 3:
            return fx::i16((p * 2) + 1);
        default:
            return fx::i16((quadrant == 2 ? -p : p) * 2);
        }
    }

    case 7: {
        const int quadrant = p >> 14;
        switch (quadrant) {
        case 1:
            return fx::i16(~(p * 2) + 0x8001);
        case 3:
            return fx::i16((p * 2) - 0x7FFE);
        default:
            return fx::i16(((quadrant == 2 ? -p : p) * 2) - 0x7FFF);
        }
    }

    default:
        if (waveform_index >= 8 && waveform_index < 0x20) {
            // Linear-interpolated wavetable, 0x81 entries per waveform.
            const int row = (waveform_index - 8) * 0x81;
            const int index = p >> 9;
            const int fraction = (p >> 1) & 0xFF;
            const int a = wave_bank_[static_cast<std::size_t>(row + index)];
            const int b = wave_bank_[static_cast<std::size_t>(row + index + 1)];
            return fx::i16((((b - a) * fraction) >> 8) + a);
        }
        return 0;
    }
}

std::pair<LfoConfig, LfoConfig> LfoEngine::configure(int tone_number,
                                                     const PartialParameters& partial) const
{
    const auto header =
        tone_.subspan(static_cast<std::size_t>(tone_number) * Tone::stride, Tone::header_size);
    const auto block = partial.raw();

    const int delay_index = read_i16(header, 0x12);

    // The pitch depth is a signed index into the cents table, negated for negative indices.
    const int pitch_index = fx::i8(block[0x15]);
    const int pitch_cents =
        pitch_index < 0 ? -cents_table_[static_cast<std::size_t>(pitch_index & 0x7F)]
                        : cents_table_[static_cast<std::size_t>(std::min(0x7F, pitch_index))];

    LfoConfig lfo1;
    lfo1.waveform = wave_map_[static_cast<std::size_t>(header[0x0E] & 0x1F)];
    lfo1.initial_phase = (header[0x0E] & 0xC0) << 8;
    lfo1.increment =
        rate_table_[static_cast<std::size_t>(std::clamp(read_u16(header, 0x10), 0, 0x7F))];
    lfo1.delay_rate = delay_table_[static_cast<std::size_t>(std::clamp(delay_index, 0, 0x7F))];
    lfo1.fade_rate = read_u16(header, 0x14);
    lfo1.pitch_depth = pitch_cents;
    // Truncation toward zero, not flooring: flooring gives -90 where the engine gives -89.
    lfo1.tvf_depth = read_i16(block, 0x34) / 2;
    lfo1.tva_depth = read_i16(block, 0x56);

    // LFO2's delay is a raw increment, and a non-positive value means INSTANT rather than never.
    // A stored -1 yields 0xffff. Treating it as zero silently disables LFO2 on every such patch.
    const int lfo2_delay = read_i16(block, 0x0A);

    LfoConfig lfo2;
    lfo2.waveform = wave_map_[static_cast<std::size_t>(block[0x06] & 0x1F)];
    lfo2.initial_phase = (block[0x06] & 0xC0) << 8;
    lfo2.increment = read_u16(block, 0x08);
    lfo2.delay_rate = lfo2_delay > 0 ? lfo2_delay : 0xFFFF;
    lfo2.fade_rate = read_u16(block, 0x0C);
    lfo2.pitch_depth = read_i16(block, 0x16);
    lfo2.tvf_depth = read_i16(block, 0x36) / 2;
    lfo2.tva_depth = read_i16(block, 0x58);

    return {lfo1, lfo2};
}

LfoRunner LfoEngine::create_runner(const LfoConfig& config) const
{
    return LfoRunner{*this, config};
}

std::pair<LfoRunner, LfoRunner> LfoEngine::create_runners(int tone_number,
                                                          const PartialParameters& partial) const
{
    const auto [lfo1, lfo2] = configure(tone_number, partial);
    return {create_runner(lfo1), create_runner(lfo2)};
}

std::vector<double>
LfoEngine::run(const LfoConfig& config, int tick_count, LfoDestination destination) const
{
    std::vector<double> output(static_cast<std::size_t>(std::max(0, tick_count)));
    LfoRunner runner = create_runner(config);

    for (double& value : output) {
        runner.tick();
        value = runner.value(destination);
    }

    return output;
}

std::vector<double> LfoEngine::modulation(int tone_number,
                                          const PartialParameters& partial,
                                          int tick_count,
                                          LfoDestination destination) const
{
    if (tick_count <= 0) {
        return {};
    }

    const auto [lfo1, lfo2] = configure(tone_number, partial);
    const std::vector<double> first = run(lfo1, tick_count - 1, destination);
    const std::vector<double> second = run(lfo2, tick_count - 1, destination);

    // The first tick is always zero: the LFO object is created after that tick's control update.
    std::vector<double> output(static_cast<std::size_t>(tick_count));
    for (std::size_t i = 0; i + 1 < output.size(); ++i) {
        output[i + 1] = first[i] + second[i];
    }

    return output;
}

std::vector<double>
LfoEngine::pitch_modulation_with_wheel(int tone_number,
                                       const PartialParameters& partial,
                                       int tick_count,
                                       std::span<const double> mod_depth_per_tick) const
{
    if (tick_count <= 0) {
        return {};
    }

    const auto [lfo1, lfo2] = configure(tone_number, partial);
    const std::vector<double> first =
        run_pitch_with_wheel(lfo1, tick_count - 1, mod_depth_per_tick);
    const std::vector<double> second = run(lfo2, tick_count - 1, LfoDestination::pitch);

    std::vector<double> output(static_cast<std::size_t>(tick_count));
    for (std::size_t i = 0; i + 1 < output.size(); ++i) {
        output[i + 1] = first[i] + second[i];
    }

    return output;
}

std::vector<double> LfoEngine::run_pitch_with_wheel(
    const LfoConfig& config, int tick_count, std::span<const double> mod_depth_per_tick) const
{
    std::vector<double> output(static_cast<std::size_t>(std::max(0, tick_count)));
    LfoRunner runner = create_runner(config);

    for (std::size_t i = 0; i < output.size(); ++i) {
        runner.tick();
        const double wheel = mod_depth_per_tick.empty()
                                 ? 0.0
                                 : mod_depth_per_tick[std::min(i, mod_depth_per_tick.size() - 1)];
        output[i] = runner.pitch_value(wheel);
    }

    return output;
}

int LfoEngine::mod_wheel_depth(int controller, int depth, int offset) noexcept
{
    // The division truncates toward zero; both operands are non-negative here in practice.
    int raw = ((depth * controller) / 4) + offset;
    raw = std::clamp(raw, -4032, 4032);

    const int magnitude = (std::abs(raw) * 2 * 48762) >> 16;
    return raw >= 0 ? magnitude : -magnitude;
}

// ---------------------------------------------------------------------------------------------
// LfoRunner
// ---------------------------------------------------------------------------------------------

void LfoRunner::tick() noexcept
{
    applied_ = false;
    if (config_.increment == 0) {
        return;
    }

    phase_ = (phase_ + config_.increment) & 0xFFFF;

    if (delay_ < 0xFFFF) {
        // The LFO runs during the delay but is not applied. A zero rate never completes, which is
        // how a patch switches an LFO off entirely.
        if (config_.delay_rate == 0) {
            return;
        }

        delay_ = std::min(0xFFFF, delay_ + config_.delay_rate);
        if (delay_ < 0xFFFF) {
            return;
        }
    }

    if (fade_ < 0xFFFF) {
        fade_ = std::min(0xFFFF, fade_ + config_.fade_rate);
    }

    applied_ = true;
}

double LfoRunner::value(LfoDestination destination) const noexcept
{
    const LfoEngine::Limits limits = LfoEngine::destination_limits(destination);
    const int depth = std::clamp(config_.depth(destination), -limits.clamp, limits.clamp);
    if (depth == 0) {
        return 0.0;
    }
    return apply(faded(depth), limits.rounding);
}

double LfoRunner::pitch_value(double wheel_depth) const noexcept
{
    const LfoEngine::Limits limits = LfoEngine::destination_limits(LfoDestination::pitch);
    const int depth = std::clamp(config_.pitch_depth, -limits.clamp, limits.clamp);

    // The wheel is summed after the fade-in, then the total is clamped to +-6000.
    const int effective =
        std::clamp(faded(depth) + static_cast<int>(wheel_depth), -limits.clamp, limits.clamp);
    return apply(effective, limits.rounding);
}

int LfoRunner::faded(int depth) const noexcept
{
    return fade_ == 0xFFFF ? depth : ((std::abs(depth) * fade_) >> 16) * (depth >= 0 ? 1 : -1);
}

double LfoRunner::apply(int effective, int rounding) const noexcept
{
    if (!applied_) {
        return 0.0;
    }

    const int wave = engine_->waveform(phase_, config_.waveform);

    // Sign-magnitude fixed-point multiply: magnitudes multiply, signs compose separately.
    const int magnitude = ((std::abs(wave) * std::abs(effective) * 2) + rounding) >> 16;
    return (wave >= 0) == (effective >= 0) ? magnitude : -magnitude;
}

} // namespace ts
