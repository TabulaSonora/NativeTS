#include "tabulasonora/envelope_machine.hpp"

#include "dsp/fixed.hpp"

#include <algorithm>

namespace ts {

EnvelopeMachine::EnvelopeMachine(const TableSet& tables)
    : rate_curve_(tables.rate_curve()),
      rate_out_(tables.env_rate_out()),
      scale_curve_(tables.env_scale_curve())
{
    const auto env_shape = tables.env_shape();
    for (std::size_t i = 0; i < shape_.size(); ++i) {
        shape_[i] = i < env_shape.size() ? env_shape[i] / 65535.0 : 1.0;
    }
}

int EnvelopeMachine::rate_scale(int base_rate, int modifier) const noexcept
{
    int u = modifier - 0x40;
    if (u == 0) {
        return unity_multiplier;
    }

    if (u < 0) {
        // Negative modifiers mirror the curve: negate the signed base and walk the other way.
        const int signed_base = fx::i8(base_rate);
        u = -u;
        base_rate = (-signed_base) & 0xFF;
        if (base_rate == 0) {
            base_rate = 0xFF;
        }
    }

    const int centred = fx::i16(base_rate - 0x80);
    if (centred == 0) {
        return unity_multiplier;
    }

    const int scale = scale_curve_[static_cast<std::size_t>(u)];

    // Kept signed so the shift stays arithmetic; on an unsigned type it would be logical and the
    // negative branch would index the wrong end of the output curve. The mask relies on two's
    // complement for a negative index, which C++20 mandates.
    const int index = centred < 0 ? (0x80 - ((centred * scale * -2) >> 8)) & 0x1FF
                                  : (((centred * scale * 2) >> 8) + 0x80) & 0x1FF;
    return rate_out_[static_cast<std::size_t>(index)];
}

int EnvelopeMachine::level_scale(int level, int modifier) const noexcept
{
    const int from_level = 0x40 - level;
    if (from_level == 0) {
        return unity_multiplier;
    }

    const int from_modifier = modifier - 0x40;
    if (from_modifier == 0) {
        return unity_multiplier;
    }

    const auto curve = [this](int at) noexcept {
        return static_cast<int>(scale_curve_[static_cast<std::size_t>(at & 0xFF)]);
    };

    int product = 0;
    if (from_level < 0) {
        if (from_modifier >= 0) {
            const int index = (0x80 - fx::shift8(-(curve(from_modifier * 2) * from_level))) & 0x1FF;
            return rate_out_[static_cast<std::size_t>(index)];
        }
        product = -(curve(-from_modifier * 2) * from_level);
    } else {
        if (from_modifier < 0) {
            const int index = (0x80 - fx::shift8(curve(-from_modifier * 2) * from_level)) & 0x1FF;
            return rate_out_[static_cast<std::size_t>(index)];
        }
        product = curve(from_modifier * 2) * from_level;
    }

    return rate_out_[static_cast<std::size_t>((fx::shift8(product) + 0x80) & 0x1FF)];
}

double EnvelopeMachine::segment_milliseconds(int rate_byte,
                                             int rate_multiplier,
                                             int velocity_multiplier,
                                             int bias) const noexcept
{
    const int index = (rate_byte & 0x7F) + bias;
    if (index < 0) {
        return 0.0;
    }

    const int ticks = rate_curve_[static_cast<std::size_t>(std::min(index, 0x7F))];
    if (ticks < minimum_segment_ticks) {
        return 0.0;
    }

    // Both products genuinely exceed 32 bits: each factor reaches 0xffff, so the engine keeps only
    // the low word and the result can come out negative. Widening either to 64 bits would change
    // the timing of every envelope that lands here.
    const int scaled = std::min(0xFFFF, fx::wmul(rate_multiplier, ticks) >> 8);
    return fx::wmul(velocity_multiplier, scaled) >> 8;
}

double EnvelopeMachine::segment_curve(double position,
                                      double start,
                                      double target,
                                      bool linear) const noexcept
{
    position = std::clamp(position, 0.0, 1.0);
    if (linear) {
        return start + ((target - start) * position);
    }

    const auto phase = static_cast<int>(position * 0xFFFF);
    const int index = phase >> 8;

    // The curve is walked backwards: entry 256 at the start of the segment down to 0 at the end,
    // and `env_ramp_segment` interpolates *upward* from the entry it lands on --
    // `shape[k] + (shape[k+1] - shape[k]) * (255 - (phase & 0xff)) / 256`, with `k = 255 - index`.
    //
    // This read the pair the other way, from `shape[k]` down to `shape[k-1]` weighted by the
    // fraction rather than its complement, which lands the whole curve one entry low: the same
    // trajectory, running about 0.4% of a segment ahead of the module's, worth a flat 0.38 dB
    // wherever the shape is steep. The table settles it -- `g_env_shape` is 258 entries, not 256,
    // and the two past the end exist for exactly the `shape[k+1]` this needs at `k = 255`.
    const auto k = static_cast<std::size_t>(std::clamp(255 - index, 0, 255));
    const double weight = static_cast<double>(255 - (phase & 0xFF)) / 256.0;
    const double remaining = shape_[k] + ((shape_[k + 1] - shape_[k]) * weight);

    return target + ((start - target) * remaining);
}

std::int64_t EnvelopeMachine::hold_samples(const PartialParameters& partial,
                                           int velocity) const noexcept
{
    const int clock = partial.raw()[0x00];
    if (clock == 0) {
        return 0;
    }
    if (clock == 0xFF) {
        return hold_forever;
    }

    const int index = clock & 0x7F;
    const int scale = level_scale(std::clamp(velocity, 0, 127), partial.raw()[0x01]);

    // The product reaches 0xffff * 0xffff, so this wraps by design before the mask takes the low
    // word back out. Values 1 and 2 compute to zero ticks and never arm.
    const int ticks =
        ((fx::wmul(scale, rate_curve_[static_cast<std::size_t>(std::min(index, 0x7F))]) >> 8)
         & 0xFFFF)
        / 10;

    return static_cast<std::int64_t>(ticks) * 320;
}

} // namespace ts
