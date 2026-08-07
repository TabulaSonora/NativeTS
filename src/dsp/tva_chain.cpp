#include "tabulasonora/tva_chain.hpp"

#include "dsp/fixed.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ts {
namespace {

/// The floor `tva_compute_env_rates` puts under segment 0 and the release.
///
/// Neither of those two stores a duration. Both store a **per-tick step** into a 0x10000 phase --
/// `0xa0000 / duration_ms`, ten times the reciprocal because a control tick is 10 ms -- and the
/// step is a `uint16` initialised to 0xffff that the division only ever replaces when the duration
/// exceeds 10. A duration of 0 does not produce an infinite step and an instant segment; it leaves
/// 0xffff standing, which walks the phase across in a shade over one tick.
///
/// Missing that made every envelope whose release rate falls off the bottom of `g_rate_curve` cut
/// its partial dead in a single sample. Audibly: `dreaming_i_was_dreaming.mid` on channel 2, whose
/// Syn.Bass 201 has two partials and where the one with the short release vanished at full gain
/// 320 samples after each note-off. Eight hard steps in the first 45 seconds where the module has
/// none, the loudest a jump of 0.535 in one sample -- a click on every note transition.
///
/// **Applied to the release only, though segment 0 saturates the same way.** Doing both is what the
/// listing says and it is measurably wrong: a 10 ms floor under the attack costs program 0 note 84
/// a fifth of its peak against the module, and four more note-oracle cases with it. What segment 0
/// has that the release does not is the second write beside the step -- `g_env_startphase_b`,
/// indexed by the *duration* rather than by 10 when it saturates, and that table is `512/n`. So a
/// short segment 0 is not simply a slow one: the phase it starts from scales with how short it is,
/// and this port models no start phase at all. Flooring the duration without that is half a
/// mechanism, and the half that hurts. The release's own start-phase write exists too, so this
/// floor is provisional on the same table -- but the release is where the audible fault was, and
/// a 320-sample fade lands its worst step at 160 against the module's 151.
[[nodiscard]] double saturating_floor_ms(double milliseconds) noexcept
{
    // `if (10 < duration)` in the engine, so 10 itself takes the floor too.
    constexpr double step_saturates_at = 10.0;

    // 0x10000 / 0xffff ticks of 10 ms. The excess over one tick is real but below a sample.
    constexpr double saturated_ms = 10.000152590219;
    return milliseconds > step_saturates_at ? milliseconds : saturated_ms;
}

} // namespace

TvaChain::TvaChain(const TableSet& tables, const EnvelopeMachine& envelope)
    : envelope_(&envelope),
      level_curve_(tables.level_curve()),
      amp_high_(tables.amp_curve_hi()),
      amp_low_(tables.amp_curve_lo()),
      velocity_curve_(tables.vel_curve()),
      scale_curve_(tables.env_scale_curve()),
      level_key_follow_(tables.kf_tva_level()),
      rate_key_follow0_(tables.kf_tva_rate0()),
      rate_key_follow1_(tables.kf_tva_rate1()),
      velocity_crossfade_(tables.vel_xfade())
{
}

double TvaChain::amp_of(int level16) const noexcept
{
    const int level = std::clamp(level16, 0, 0xFFFF);

    // Widening site 1 of 4. The two table entries multiply to more than 32 bits before the shift.
    const std::int64_t product =
        static_cast<std::int64_t>(amp_high_[static_cast<std::size_t>(level >> 8)])
        * amp_low_[static_cast<std::size_t>(level & 0xFF)];
    return static_cast<double>(product >> 16) / 65535.0;
}

std::optional<int> TvaChain::partial_level(const PartialParameters& partial,
                                           int velocity) const noexcept
{
    const auto raw = partial.raw();
    const int low = raw[0x4F];
    const int high = raw[0x51];
    const int window_low = std::min(low, high);
    const int window_high = std::max(low, high);

    if (velocity < window_low || velocity > window_high) {
        return std::nullopt;
    }

    int position = 0;
    if (window_high == window_low) {
        position = 0x7F;
    } else {
        const int offset = velocity - window_low;
        const int widened = ((offset * 0x100) | (offset >> 8)) & 0xFFFF;
        const int scaled = widened / (window_high - window_low);
        position = scaled > 0xFF ? 0x7F : scaled >> 1;
    }

    const int span = fx::i8((raw[0x52] - raw[0x50]) & 0xFF);

    // A window stored high-to-low crossfades the other way round.
    const int index = low <= high ? position : (window_low - position) & 0xFF;

    const int curve = velocity_crossfade_[static_cast<std::size_t>(
        (partial.velocity_crossfade_curve() * 0x80) + index)];

    // Truncation to a signed byte is load-bearing: curve reaches 255 and |span| 127, so the
    // doubled product overshoots and comes back as a negative delta.
    const int delta = fx::i8(((curve * std::abs(span)) + 0x7F) * 2 >> 8);

    const int level = fx::i8((raw[0x50] + (span >= 0 ? delta : -delta)) & 0xFF);
    return level == 0 ? 1 : level;
}

int TvaChain::base_level(const PartialParameters& partial,
                         int partial_level_byte,
                         int key,
                         int zone_level,
                         int tone_level) const noexcept
{
    const auto raw = partial.raw();
    int level = 0xFFFF - level_curve_[raw[0x53]];

    const int key_follow =
        fx::i8(level_key_follow_[static_cast<std::size_t>((raw[0x54] * 0x80) + (key & 0x7F))]);
    const int depth = raw[0x55] - 0x40;

    if (depth != 0 && key_follow != 0) {
        const int scale = scale_curve_[static_cast<std::size_t>(std::abs(depth) & 0xFF)];
        const int term = velocity_curve_[static_cast<std::size_t>(
                             ((std::abs(key_follow) * scale * 2) >> 8) & 0xFF)]
                         * 0x100;

        // Matching signs reinforce, mixed signs oppose.
        const bool same_sign = (depth < 0 && key_follow < 0) || (depth >= 0 && key_follow >= 0);
        level = same_sign ? std::min(0xFFFF, level + term) : level - term;
        if (level < 1) {
            level = 1;
        }
    }

    // Four attenuations subtracted from a base, clamped to 1 after each. Dropping the last two once
    // left a 1.15-2.06x per-voice residual against the engine's own gain word.
    const std::array<int, 3> attenuations{
        level_curve_[static_cast<std::size_t>(std::clamp(partial_level_byte, 0, 127))],
        level_curve_[static_cast<std::size_t>(std::clamp(zone_level, 0, 127))],
        level_curve_[static_cast<std::size_t>(std::clamp(tone_level, 0, 127))],
    };

    for (int attenuation : attenuations) {
        level -= attenuation;
        if (level < 1) {
            level = 1;
        }
    }

    return level;
}

SegmentEnvelope TvaChain::create_envelope(const PartialParameters& partial,
                                          int velocity,
                                          int key,
                                          int zone_level,
                                          int tone_level,
                                          int sample_rate,
                                          double attack_milliseconds,
                                          std::optional<int> rate_key,
                                          const PartModifiers& modifiers) const
{
    const auto raw = partial.raw();
    const int level_byte = partial_level(partial, velocity).value_or(velocity);
    const int base = base_level(partial, level_byte, key, zone_level, tone_level);

    // Segment targets in the gain domain. A stage whose attenuation exceeds the base is exact
    // silence, not the amplitude table's floor -- the engine's gain word reads 0.000000 there.
    std::array<double, SegmentEnvelope::segment_count> targets{};
    for (std::size_t i = 0; i < targets.size(); ++i) {
        const int level = base - level_curve_[raw[0x5A + i]];
        targets[i] = level <= 0 ? 0.0 : amp_scale * amp_of(level);
    }

    // Two key-follow tables, not one. The engine reads g_kf_tvarate0 for the four main segments and
    // g_kf_tvarate1 for the release -- see tva_compute_env_rates, where the two calls to
    // env_rate_scale index different bases with block[0x65] and block[0x66].
    //
    // Both index by voice+0x161, which is not the MIDI key on a drum part -- see rate_key.
    const int index = rate_key.value_or(key);
    const int main_rate = envelope_->rate_scale(
        (rate_key_follow0_[static_cast<std::size_t>((raw[0x65] * 0x80) + index)] - 0x80) & 0xFF,
        raw[0x67]);
    const int release_rate = envelope_->rate_scale(
        (rate_key_follow1_[static_cast<std::size_t>((raw[0x66] * 0x80) + index)] - 0x80) & 0xFF,
        raw[0x68]);

    // Two velocity level-scales, not one: segments 0-1 use 0x69, segments 2-3 and the release use
    // 0x6a. Sharing one makes the later segments descend far too steeply.
    const int velocity_early = envelope_->level_scale(level_byte, raw[0x69]);
    const int velocity_late = envelope_->level_scale(level_byte, raw[0x6A]);

    std::array<double, SegmentEnvelope::segment_count> segment_samples{};
    std::array<bool, SegmentEnvelope::segment_count> linear{};

    // The part's envelope offsets bias the rate-curve index, and the three of them divide the
    // envelope the same way the two velocity scales do: attack covers segments 0 and 1, decay
    // covers 2 and 3, release covers the release. `tva_compute_env_rates` recomputes its bias
    // between segments 1 and 2 and again before the release, which is where the split comes from.
    for (std::size_t i = 0; i < segment_samples.size(); ++i) {
        double seconds = envelope_->segment_milliseconds(raw[0x5E + i],
                                                         main_rate,
                                                         i < 2 ? velocity_early : velocity_late,
                                                         i < 2 ? modifiers.attack_bias()
                                                               : modifiers.decay_bias())
                         / 1000.0;

        if (i == 0 && attack_milliseconds > 0) {
            seconds = std::max(seconds, attack_milliseconds / 1000.0);
        }

        segment_samples[i] = seconds * sample_rate;
        linear[i] = EnvelopeMachine::is_linear_segment(raw[0x5E + i]);
    }

    const double release_ms = saturating_floor_ms(envelope_->segment_milliseconds(
        raw[0x62], release_rate, velocity_late, modifiers.release_bias()));

    return SegmentEnvelope{
        *envelope_,
        targets,
        segment_samples,
        linear,
        /*release_target=*/0.0,
        /*release_samples=*/release_ms / 1000.0 * sample_rate,
        /*release_linear=*/EnvelopeMachine::is_linear_segment(raw[0x62]),
        /*after_release=*/0.0,
        /*control_tick_samples=*/sample_rate / control_tick_hz,
    };
}

std::vector<float> TvaChain::render(const PartialParameters& partial,
                                    int velocity,
                                    int key,
                                    double hold_seconds,
                                    double tail_seconds,
                                    int zone_level,
                                    int tone_level,
                                    int sample_rate,
                                    double attack_milliseconds,
                                    std::optional<int> rate_key) const
{
    const auto sample_count = static_cast<int>((hold_seconds + tail_seconds) * sample_rate);
    if (sample_count <= 0) {
        return {};
    }

    SegmentEnvelope envelope = create_envelope(
        partial, velocity, key, zone_level, tone_level, sample_rate, attack_milliseconds, rate_key);

    envelope.note_off(std::min(static_cast<int>(hold_seconds * sample_rate), sample_count));

    std::vector<float> gain(static_cast<std::size_t>(sample_count));
    for (std::size_t n = 0; n < gain.size(); ++n) {
        gain[n] = static_cast<float>(envelope.value_at(static_cast<std::int64_t>(n)));
    }

    return gain;
}

double TvaChain::part_volume_scale(int volume, int expression, int master) noexcept
{
    const auto scale = [](int a, int b, int m) noexcept {
        const int u = ((((b * a) & 0xFFFF) * m) >> 6) & 0xFFFF;

        // Widening site 2 of 4. The intermediate exceeds 32 bits before the shift.
        const std::int64_t squared = (static_cast<std::int64_t>(u) * 0x10410) >> 16;
        return static_cast<double>(squared) * static_cast<double>(squared);
    };

    const double reference = scale(127, 127, 127);
    return scale(std::clamp(volume, 0, 127),
                 std::clamp(expression, 0, 127),
                 std::clamp(master, 0, 127))
           / reference;
}

int TvaChain::part_volume_word(int volume, int expression, int master) noexcept
{
    const auto v = static_cast<std::uint32_t>(std::clamp(volume, 0, 127));
    const auto e = static_cast<std::uint32_t>(std::clamp(expression, 0, 127));
    const auto m = static_cast<std::uint32_t>(std::clamp(master, 0, 127));

    const std::uint32_t u = ((((e * v) & 0xFFFFU) * m) >> 6) & 0xFFFFU;
    const std::uint32_t level = (u * 0x10410U) >> 16;

    // The engine tests the narrowed half, not the whole word: a level that reads zero as an `int16`
    // is silence and never reaches the squaring.
    if (static_cast<std::int16_t>(level) == 0) {
        return 0;
    }

    const std::uint32_t squared = level * level;
    const auto folded = static_cast<std::uint32_t>(static_cast<std::uint16_t>(squared >> 16));
    return static_cast<int>(std::min<std::uint32_t>((folded * 0x208U) >> 8, 0xFFFFU));
}

} // namespace ts
