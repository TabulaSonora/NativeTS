#include "tabulasonora/tvf_chain.hpp"

#include "dsp/fixed.hpp"
#include "tabulasonora/tva_chain.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace ts {

TvfChain::TvfChain(const TableSet& tables, const EnvelopeMachine& envelope)
    : envelope_machine_(&envelope),
      env_depth_(tables.tvf_env_depth()),
      pitch_env_(tables.pitch_env()),
      env_depth_key_follow_(tables.kf_tvf_env()),
      reso_curve_(tables.reso_curve()),
      rate_key_follow_(tables.kf_tvf_rate()),
      filter_type_coef_(tables.filter_type_coef()),
      warp_(tables.tvf_cutoff_warp()),
      ceiling_(tables.tvf_cutoff_ceil()),
      q_low_pass_(tables.tvf_q_lp()),
      q_type6_(tables.tvf_q_t6()),
      ramp_exp_(tables.ramp_exp()),
      velocity_sensitivity_(tables.vel_sens())
{
}

FilterTap TvfChain::tap(int filter_type) const noexcept
{
    const int type = filter_type & 0xFF;
    if (type > 2 && ((type - 4) & 0xFF) > 2) {
        return FilterTap::bypass;
    }

    switch (filter_type_coef_[static_cast<std::size_t>(type)] & 0xC00) {
    case 0x000:
        return FilterTap::low_pass;
    case 0x400:
        return FilterTap::band_pass;
    case 0x800:
        return FilterTap::high_pass;
    default:
        return FilterTap::notch;
    }
}

int TvfChain::resonance_byte(const PartialParameters& partial,
                             int part_resonance,
                             int part_resonance_default) noexcept
{
    const int value = (2 * (0x80 - part_resonance_default - part_resonance)) + partial.resonance();
    return std::max(4, std::min(0x7F, std::max(0, value)));
}

int TvfChain::cutoff_units(double cutoff15, int resonance_byte_value) const noexcept
{
    const auto c = static_cast<int>(std::clamp(cutoff15, 0.0, static_cast<double>(0x7FFF)));
    const int high = c >> 8;
    const int low = c & 0xFF;

    int value = warp_[static_cast<std::size_t>(high)];
    if (low != 0) {
        value += ((warp_[static_cast<std::size_t>(high + 1)] - value) * low) >> 8;
    }

    return std::min(value,
                    static_cast<int>(ceiling_[static_cast<std::size_t>(resonance_byte_value)]))
           << 2;
}

double TvfChain::frequency_coefficient(int units) const noexcept
{
    const int x = std::clamp(units, 0, 0x3FFFF);
    const int index = (x >> 6) & 0xFF;
    const int fraction = x & 0x3F;

    // Widening site 3 of 4. The table entries reach 2^18, so the interpolation overflows 32 bits
    // before the shift.
    std::int64_t value =
        ((static_cast<std::int64_t>(ramp_exp_[static_cast<std::size_t>(index)]) * (0x40 - fraction))
         + (static_cast<std::int64_t>(ramp_exp_[static_cast<std::size_t>(index + 1)]) * fraction))
        >> 6;
    value >>= 0xF - ((x >> 0xE) & 0xF);

    return static_cast<double>(std::min<std::int64_t>(0x7FFF, value >> 3)) / 16384.0;
}

double
TvfChain::damping_coefficient(int units, int resonance_byte_value, int filter_type) const noexcept
{
    int raw = 0;
    if (filter_type == 0 && resonance_byte_value == 0x40) {
        raw = q_low_pass_[static_cast<std::size_t>((units >> 2) >> 8)] << 2;
    } else if (filter_type == 6) {
        raw = q_type6_[static_cast<std::size_t>((units >> 2) >> 8)] << 2;
    } else {
        raw = resonance_byte_value << 11;
    }

    return raw / static_cast<double>(0x20000);
}

double
TvfChain::cutoff_hz(double cutoff15, int resonance_byte_value, int sample_rate) const noexcept
{
    const double f = frequency_coefficient(cutoff_units(cutoff15, resonance_byte_value));
    return sample_rate / std::numbers::pi * std::asin(std::min(1.0, f / 2.0));
}

int TvfChain::effective_velocity(const PartialParameters& partial, int velocity) const noexcept
{
    return velocity_sensitivity_[static_cast<std::size_t>((partial.velocity_curve() * 0x80)
                                                          + std::clamp(velocity, 0, 0x7F))];
}

TvfChain::Offsets
TvfChain::envelope_offsets(const PartialParameters& partial, int key, int velocity) const
{
    const auto raw = partial.raw();

    const int key_follow =
        env_depth_key_follow_[static_cast<std::size_t>(((raw[0x32] & 0x0F) * 0x80) + (key & 0x7F))];
    const int depth = raw[0x33];

    int peak = 0;
    if (depth < 0x40) {
        const int value = env_depth_word(env_depth_low_branch + (0x80 - (depth * 2)));
        // Negate first so the shift stays on a positive value: an arithmetic right shift of a
        // negative rounds toward minus infinity and would bias the peak by one unit.
        peak = key_follow >= 0 ? -((key_follow * value) >> 9) : (-key_follow * value) >> 9;
    } else {
        const int value = env_depth_word(env_depth_high_branch + (depth * 2));
        peak = key_follow >= 0 ? (key_follow * value) >> 9 : -((-key_follow * value) >> 9);
    }

    // Resonance scaling of the envelope depth. Note block[0x4a] is this scaler, not resonance --
    // that is block[0x30].
    const int resonance_scale = raw[0x4A] - 0x40;
    int c0 = 0;
    int negated = 0;
    if (resonance_scale == 0) {
        c0 = 0x7FFF;
    } else {
        const int s = 0x7F - effective_velocity(partial, velocity);
        c0 = resonance_scale < 0
                 ? (reso_curve_[static_cast<std::size_t>(std::abs(resonance_scale) & 0x3F)] * s)
                       - 0x7FFF
                 : 0x7FFF - (reso_curve_[static_cast<std::size_t>(resonance_scale & 0x3F)] * s);

        if (c0 < 0) {
            c0 = -c0;
            negated = 1;
        }
    }

    const int multiplier = raw[0x38] | (raw[0x39] << 8);

    const auto convert = [&](int level) {
        const int offset = level - 0x40;
        const int magnitude = std::abs(offset);
        if (magnitude == 0) {
            return peak;
        }

        const int flag = offset >= 0 ? negated : ~negated & 1;

        // multiplier * c0 overflows 32 bits by design; the 31-bit mask is a sign-strip of the
        // wrapped value, not a truncation to 32 bits.
        const int delta =
            ((fx::wmul(multiplier, c0) & 0x7FFFFFFF) >> 15) * pitch_bias(magnitude) >> 7;
        return flag == 0 ? peak + delta : peak - delta;
    };

    Offsets offsets;
    offsets.peak = peak;
    for (std::size_t i = 0; i < offsets.segments.size(); ++i) {
        offsets.segments[i] = convert(raw[0x3A + i]) - peak;
    }
    offsets.release = convert(raw[0x3E]) - peak;
    return offsets;
}

TvfChain::Envelope TvfChain::create_envelope(const PartialParameters& partial,
                                             int velocity,
                                             int key,
                                             int sample_rate,
                                             const PartModifiers& modifiers) const
{
    const auto raw = partial.raw();
    const Offsets offsets = envelope_offsets(partial, key, velocity);

    // The filter envelope opts in to the part's envelope offsets; the amplitude envelope does not
    // get the choice. A partial with the bit clear takes a bias of zero, not a bias it ignores --
    // which is the same thing here, and worth saying because the two are not the same in the
    // engine: `tvf_compute_env_rates` computes the bias only inside the branch.
    const bool follows = responds_to_env_modifiers(partial);
    const int attack_bias = follows ? modifiers.attack_bias() : 0;
    const int decay_bias = follows ? modifiers.decay_bias() : 0;
    const int release_bias = follows ? modifiers.release_bias() : 0;

    const int main_rate = envelope_machine_->rate_scale(
        (rate_key_follow_[static_cast<std::size_t>((raw[0x46] * 0x80) + (key & 0x7F))] - 0x80)
            & 0xFF,
        raw[0x48]);

    // Two velocity level-scales, as in the TVA: 0x4b for segments 0-1, 0x4c for 2-3 and release.
    // Sharing 0x4b makes the later segments run about 1.45x too fast on patches where they differ,
    // closing the filter early in the sustain.
    const int velocity_early = envelope_machine_->level_scale(velocity, raw[0x4B]);
    const int velocity_late = envelope_machine_->level_scale(velocity, raw[0x4C]);

    std::array<double, SegmentEnvelope::segment_count> targets{};
    std::array<double, SegmentEnvelope::segment_count> segment_samples{};
    std::array<bool, SegmentEnvelope::segment_count> linear{};

    for (std::size_t i = 0; i < targets.size(); ++i) {
        const double milliseconds =
            envelope_machine_->segment_milliseconds(raw[0x3F + i],
                                                    main_rate,
                                                    i < 2 ? velocity_early : velocity_late,
                                                    i < 2 ? attack_bias : decay_bias);

        targets[i] = offsets.segments[i];

        // Every TVF segment occupies at least 2 ms, so an instant one still slews rather than
        // stepping the coefficients.
        segment_samples[i] = std::max(milliseconds / 1000.0, 0.002) * sample_rate;
        linear[i] = true;
    }

    // The release has its own rate row and modifier -- 0x47/0x49, not the main 0x46/0x48.
    const int release_rate = envelope_machine_->rate_scale(
        (rate_key_follow_[static_cast<std::size_t>((raw[0x47] * 0x80) + (key & 0x7F))] - 0x80)
            & 0xFF,
        raw[0x49]);
    const double release_ms = envelope_machine_->segment_milliseconds(
        raw[0x43], release_rate, velocity_late, release_bias);

    SegmentEnvelope envelope{
        *envelope_machine_,
        targets,
        segment_samples,
        linear,
        /*release_target=*/static_cast<double>(offsets.release),
        /*release_samples=*/std::max(release_ms / 1000.0 * sample_rate, 1e-9),
        /*release_linear=*/true,
        /*after_release=*/static_cast<double>(offsets.release),
        /*control_tick_samples=*/sample_rate / TvaChain::control_tick_hz,
    };

    // Stage B: the part's cutoff offset joins the base and the envelope peak. A controller step is
    // a whole 0x100 of the 15-bit sum, so CC#74 walks the cutoff in coarse strides.
    //
    // The low clamp lives downstream, where the per-tick sum of base, envelope and LFO is clamped
    // to [0, 0x7fff] exactly as the engine clamps it -- so a cutoff offset that drives this
    // negative is right to stay negative here. The high clamp is kept where it was.
    return Envelope{
        std::move(envelope),
        std::min(0x7FFF, (raw[0x2F] * 0x100) + offsets.peak + modifiers.cutoff_offset())};
}

std::vector<double> TvfChain::envelope(const PartialParameters& partial,
                                       int velocity,
                                       int key,
                                       double hold_seconds,
                                       double tail_seconds,
                                       int sample_rate) const
{
    const auto sample_count = static_cast<int>((hold_seconds + tail_seconds) * sample_rate);
    if (sample_count <= 0) {
        return {};
    }

    Envelope built = create_envelope(partial, velocity, key, sample_rate);
    built.offsets.note_off(std::min(static_cast<int>(hold_seconds * sample_rate), sample_count));

    std::vector<double> offsets(static_cast<std::size_t>(sample_count));
    for (std::size_t n = 0; n < offsets.size(); ++n) {
        offsets[n] =
            std::clamp(built.base_cutoff + built.offsets.value_at(static_cast<std::int64_t>(n)),
                       0.0,
                       static_cast<double>(0x7FFF));
    }

    return offsets;
}

void TvfChain::apply(std::span<float> signal,
                     std::span<const double> cutoff15,
                     int filter_type,
                     int resonance_byte_value,
                     int block_samples) const noexcept
{
    const FilterTap selected = tap(filter_type);
    if (selected == FilterTap::bypass) {
        return;
    }

    StateVariableFilter filter;

    for (std::size_t start = 0; start < signal.size();
         start += static_cast<std::size_t>(block_samples)) {
        const std::size_t end =
            std::min(start + static_cast<std::size_t>(block_samples), signal.size());

        // The mean over the block the coefficients will serve, not the value at the tick: the
        // envelope can cross several segments inside one 10 ms tick, and a single sample point
        // costs about 1.7% of peak on a piano attack.
        double sum = 0.0;
        for (std::size_t n = start; n < end; ++n) {
            sum += cutoff15[n];
        }

        const int units =
            cutoff_units(sum / static_cast<double>(end - start), resonance_byte_value);
        const double f = frequency_coefficient(units);
        const double q = damping_coefficient(units, resonance_byte_value, filter_type);

        for (std::size_t n = start; n < end; ++n) {
            // The filter runs in double and the signal is float; the widening and the single
            // narrowing back are both deliberate, so they are spelled out.
            signal[n] =
                static_cast<float>(filter.process(static_cast<double>(signal[n]), f, q, selected));
        }
    }
}

int TvfChain::env_depth_word(int offset) const
{
    if (offset < 0 || static_cast<std::size_t>(offset) + 1 >= env_depth_.size()) {
        throw std::runtime_error("TVF env-depth read at offset " + std::to_string(offset)
                                 + " is outside the " + std::to_string(env_depth_.size())
                                 + "-byte table.");
    }
    return env_depth_[static_cast<std::size_t>(offset)]
           | (env_depth_[static_cast<std::size_t>(offset) + 1] << 8);
}

int TvfChain::pitch_bias(int magnitude) const
{
    const int offset = pitch_bias_offset + (magnitude & 0xFF);
    if (static_cast<std::size_t>(offset) >= pitch_env_.size()) {
        throw std::runtime_error("Pitch-bias read at offset " + std::to_string(offset)
                                 + " is outside the " + std::to_string(pitch_env_.size())
                                 + "-byte table.");
    }
    return pitch_env_[static_cast<std::size_t>(offset)];
}

} // namespace ts
