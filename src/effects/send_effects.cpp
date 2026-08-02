#include "tabulasonora/send_effects.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ts {

// ---------------------------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------------------------

Reverb Reverb::for_type(std::optional<int> type)
{
    const ReverbPresets& presets = EffectPresets::defaults().reverb();
    if (!type) {
        return Reverb{presets.defaults};
    }
    if (*type < 0 || static_cast<std::size_t>(*type) >= presets.types.size()) {
        throw std::out_of_range("Reverb type must be 0-7.");
    }
    return Reverb{presets.types[static_cast<std::size_t>(*type)]};
}

void Reverb::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0);
    dc_state_ = 0.0;
    damp_ = 0.0;
    tank_state_a_ = 0.0;
    tank_state_b_ = 0.0;
    write_cursor_ = 0;
}

void Reverb::process(std::span<const float> input, std::span<float> left, std::span<float> right)
{
    process(input, {}, {}, left, right);
}

void Reverb::process(std::span<const float> input,
                     std::span<const float> chorus_left,
                     std::span<const float> chorus_right,
                     std::span<float> left,
                     std::span<float> right)
{
    const ReverbPreset& p = preset_;
    const TankTaps& tank_a = p.tank_a.taps;
    const TankTaps& tank_b = p.tank_b.taps;
    const TankAllpasses& nested = p.tank_allpasses;

    const auto at = [this](int tap) noexcept -> double& {
        return ring_[static_cast<std::size_t>((tap + write_cursor_) & ring_mask)];
    };

    for (std::size_t i = 0; i < input.size(); ++i) {
        const double wet = static_cast<double>(input[i])
                           + (i < chorus_left.size() ? static_cast<double>(chorus_left[i]) : 0.0)
                           + (i < chorus_right.size() ? static_cast<double>(chorus_right[i]) : 0.0);

        // Input conditioner: the DC blocker, then a one-pole damping filter.
        const double x = (wet * DcBlocker::input_coefficient) + dc_state_;
        dc_state_ += x * DcBlocker::state_coefficient;
        damp_ = (damp_ * p.damp_feedback) + (p.damp_input * x);
        at(input_tap) = damp_ * p.gain_input;

        // Four series allpass diffusers.
        const double injection = at(p.injection_tap) * p.gain_injection;
        double previous_write = 0.0;
        double previous_read = 0.0;
        for (std::size_t k = 0; k < p.diffusers.size(); ++k) {
            const AllpassStage& stage = p.diffusers[k];
            const double read = at(stage.read_tap);
            const double write = k == 0 ? (read * stage.coef_a) + injection
                                        : (read * stage.coef_a)
                                              + (previous_write * p.diffusers[k - 1].coef_b)
                                              + previous_read;

            at(stage.write_tap) = write;
            previous_write = write;
            previous_read = read;
        }

        const double carry = (previous_write * p.diffusers.back().coef_b) + previous_read;

        run_tank(tank_a, p.tank_a, nested.a0, nested.a1, carry, p.gain_feedback, tank_state_a_);
        run_tank(tank_b, p.tank_b, nested.b0, nested.b1, carry, p.gain_feedback, tank_state_b_);

        // Output taps are latched before the cursor moves.
        left[i] = static_cast<float>(
            (at(tank_a.tap28) + at(tank_a.tap20) + at(tank_b.tap28) + at(tank_b.tap20))
            * p.gain_output);

        right[i] = static_cast<float>(
            (at(tank_a.tap2c) + at(tank_a.tap24) + at(tank_b.tap2c) + at(tank_b.tap24))
            * p.gain_output);

        // The write cursor decrements, which is why every tap offset is added to it.
        write_cursor_ = (write_cursor_ - 1) & ring_mask;
    }
}

void Reverb::run_tank(const TankTaps& taps,
                      const ReverbTank& tank,
                      const AllpassStage& first,
                      const AllpassStage& second,
                      double carry,
                      double feedback,
                      double& state) noexcept
{
    const auto at = [this](int tap) noexcept -> double& {
        return ring_[static_cast<std::size_t>((tap + write_cursor_) & ring_mask)];
    };

    const double lowpass = (at(taps.tap1c) * tank.coef_b) + (tank.coef_a * state);
    state = lowpass;

    const double a = at(first.read_tap);
    const double p = (lowpass * feedback) + carry + (a * first.coef_a);
    at(first.write_tap) = p;
    at(taps.tap10) = (p * first.coef_b) + a;

    const double b = at(second.read_tap);
    const double q = (b * second.coef_a) + at(taps.tap14);
    at(second.write_tap) = q;
    at(taps.tap18) = (q * second.coef_b) + b;
}

// ---------------------------------------------------------------------------------------------
// Chorus
// ---------------------------------------------------------------------------------------------

Chorus Chorus::for_type(std::optional<int> type)
{
    const ChorusPresets& presets = EffectPresets::defaults().chorus();
    if (!type) {
        return Chorus{presets.defaults};
    }
    if (*type < 0 || static_cast<std::size_t>(*type) >= presets.types.size()) {
        throw std::out_of_range("Chorus type must be 0-7.");
    }
    return Chorus{presets.types[static_cast<std::size_t>(*type)]};
}

void Chorus::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0);
    phase_ = 0;
    dc_state_ = 0.0;
    lowpass_ = 0.0;
    feedback_ = 0.0;
    write_cursor_ = 0;
}

void Chorus::process(std::span<const float> input, std::span<float> left, std::span<float> right)
{
    const ChorusPreset& p = preset_;

    for (std::size_t i = 0; i < input.size(); ++i) {
        phase_ = (phase_ + p.lfo_increment) & (phase_sign - 1);
        const int signed_phase = phase_ >= phase_half ? phase_ - phase_sign : phase_;

        const double x = (static_cast<double>(input[i]) * DcBlocker::input_coefficient) + dc_state_;
        dc_state_ += x * DcBlocker::state_coefficient;
        lowpass_ = (lowpass_ * p.lpf_a) + (p.lpf_b * x);

        ring_[static_cast<std::size_t>(write_cursor_ & ring_mask)] =
            ((feedback_ * p.feedback) + lowpass_) * p.gain_write;

        const double wet1 = read_tap(p.tap1_depth, p.tap1_base, std::abs(signed_phase));

        // The second tap runs 180 degrees out of phase with the first.
        const int opposite = (phase_ - phase_half) & (phase_sign - 1);
        const int signed_opposite = opposite >= phase_half ? opposite - phase_sign : opposite;
        const double wet2 = read_tap(p.tap2_depth, p.tap2_base, std::abs(signed_opposite));

        write_cursor_ = (write_cursor_ - 1) & ring_mask;
        feedback_ = wet1;

        left[i] = static_cast<float>(wet1 * p.gain_tap);
        right[i] = static_cast<float>(wet2 * p.gain_tap);
    }
}

double Chorus::read_tap(int depth, int base_delay, int triangle) const noexcept
{
    // The delay offset is 12.12 fixed point, linearly interpolated between adjacent samples.
    //
    // Widening site 4 of 4, and the one found by ear rather than by inspection: the triangle
    // reaches 2^23 and the depth several thousand, so a 32-bit multiply wraps and lands the tap at
    // the wrong delay entirely -- one channel at a different delay from the other.
    const auto offset =
        static_cast<int>(((static_cast<std::int64_t>(depth) * triangle) >> 14) + base_delay);
    const int index = (offset >> 12) + write_cursor_;
    const double fraction = (0x1000 - (offset & 0xFFF)) / 4096.0;

    return ((1.0 - fraction) * ring_[static_cast<std::size_t>((index + 1) & ring_mask)])
           + (fraction * ring_[static_cast<std::size_t>(index & ring_mask)]);
}

// ---------------------------------------------------------------------------------------------
// SystemDelay
// ---------------------------------------------------------------------------------------------

SystemDelay::SystemDelay(const DelayParameters& parameters) : parameters_(parameters)
{
    pre_low_pass_coefficient_ =
        (parameters.pre_low_pass >= 0
         && static_cast<std::size_t>(parameters.pre_low_pass)
                < DelayPresets::pre_low_pass_coefficients.size())
            ? DelayPresets::pre_low_pass_coefficients[static_cast<std::size_t>(
                  parameters.pre_low_pass)]
            : 0.0;

    pending_left_.assign(static_cast<std::size_t>(pre_delay_), 0.0F);
    pending_right_.assign(static_cast<std::size_t>(pre_delay_), 0.0F);
}

SystemDelay SystemDelay::for_type(int type)
{
    const DelayPresets& presets = EffectPresets::defaults().delay();
    if (type < 0 || static_cast<std::size_t>(type) >= presets.raw_presets.size()) {
        throw std::out_of_range("Delay type must be 0-9.");
    }
    return SystemDelay{compile(presets.raw_presets[static_cast<std::size_t>(type)])};
}

double SystemDelay::time_milliseconds(int raw)
{
    const DelayPresets& presets = EffectPresets::defaults().delay();
    return presets.time_milliseconds[static_cast<std::size_t>(std::clamp(raw, 1, 115) - 1)];
}

double SystemDelay::ratio_percent(int raw)
{
    const DelayPresets& presets = EffectPresets::defaults().delay();
    return presets.ratio_percent[static_cast<std::size_t>(std::clamp(raw, 1, 120) - 1)];
}

DelayParameters SystemDelay::compile(std::span<const int> raw, int sample_rate)
{
    if (raw.size() < 10) {
        throw std::invalid_argument("A delay preset row needs ten parameters.");
    }

    // Round-half-to-even, matching .NET's Math.Round. Unlike the SMF event grid there is no floor
    // after this to absorb the difference, so a tap length really can land one sample either side.
    const auto round_even = [](double value) { return static_cast<int>(std::nearbyint(value)); };

    const int centre = round_even(time_milliseconds(raw[1]) * sample_rate / 1000.0);
    const int left = round_even(centre * ratio_percent(raw[2]) / 100.0);
    const int right = round_even(centre * ratio_percent(raw[3]) / 100.0);

    // The return level scales the whole wet output and is kept separate from the send.
    const double return_level = raw[7] / 127.0;

    return DelayParameters{
        .centre_samples = std::max(1, centre),
        .left_samples = std::max(1, left),
        .right_samples = std::max(1, right),
        .centre_gain = raw[4] / 127.0 * return_level,
        .left_gain = raw[5] / 127.0 * return_level,
        .right_gain = raw[6] / 127.0 * return_level,
        // Raw 0-127 maps to -1..+1; the display range is -64..+63.
        .feedback = (raw[8] - 64) / 64.0,
        .pre_low_pass = raw[0],
        .send_to_reverb = raw[9] / 127.0,
    };
}

void SystemDelay::reset()
{
    std::fill(ring_.begin(), ring_.end(), 0.0);
    std::fill(pending_left_.begin(), pending_left_.end(), 0.0F);
    std::fill(pending_right_.begin(), pending_right_.end(), 0.0F);
    pending_ = 0;
    lowpass_ = 0.0;
    write_cursor_ = 0;
}

void SystemDelay::process(std::span<const float> input,
                          std::span<float> left,
                          std::span<float> right)
{
    const DelayParameters& p = parameters_;

    for (std::size_t i = 0; i < input.size(); ++i) {
        const double centre =
            ring_[static_cast<std::size_t>((write_cursor_ - p.centre_samples) & ring_mask)];

        double feedback_source = centre;
        if (pre_low_pass_coefficient_ != 0.0) {
            lowpass_ = (lowpass_ * pre_low_pass_coefficient_)
                       + ((1.0 - pre_low_pass_coefficient_) * centre);
            feedback_source = lowpass_;
        }

        ring_[static_cast<std::size_t>(write_cursor_)] =
            static_cast<double>(input[i]) + (p.feedback * feedback_source);

        const double l =
            ring_[static_cast<std::size_t>((write_cursor_ - p.left_samples) & ring_mask)];
        const double r =
            ring_[static_cast<std::size_t>((write_cursor_ - p.right_samples) & ring_mask)];

        const auto wet_left = static_cast<float>((p.left_gain * l) + (p.centre_gain * centre));
        const auto wet_right = static_cast<float>((p.right_gain * r) + (p.centre_gain * centre));

        write_cursor_ = (write_cursor_ + 1) & ring_mask;

        // Apply the fixed input pre-delay as an equivalent output shift.
        if (pending_ < pre_delay_) {
            pending_left_[static_cast<std::size_t>(pending_)] = wet_left;
            pending_right_[static_cast<std::size_t>(pending_)] = wet_right;
            ++pending_;
            left[i] = 0.0F;
            right[i] = 0.0F;
        } else {
            const auto slot =
                static_cast<std::size_t>((pending_ - pre_delay_) % std::max(1, pre_delay_));
            left[i] = pending_left_[slot];
            right[i] = pending_right_[slot];
            pending_left_[slot] = wet_left;
            pending_right_[slot] = wet_right;
            ++pending_;
        }
    }
}

} // namespace ts
