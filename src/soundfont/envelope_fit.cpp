#include "tabulasonora/soundfont_envelopes.hpp"

#include <algorithm>
#include <cmath>

namespace ts::sf2 {
namespace {

/// Attenuation in centibels for a gain ratio, clamped to the floor.
[[nodiscard]] double to_centibels(double ratio) noexcept
{
    if (ratio <= 0.0) {
        return cb_silence;
    }
    return std::clamp(-200.0 * std::log10(ratio), 0.0, cb_silence);
}

[[nodiscard]] double gain_of(double centibels) noexcept
{
    return std::pow(10.0, -centibels / 200.0);
}

} // namespace

int to_timecents(double seconds) noexcept
{
    if (!(seconds > 0.0)) {
        return min_timecents;
    }
    const double tc = 1200.0 * std::log2(seconds);
    return static_cast<int>(
        std::lround(std::clamp(tc, static_cast<double>(min_timecents),
                               static_cast<double>(max_timecents))));
}

double from_timecents(int timecents) noexcept
{
    return std::pow(2.0, static_cast<double>(timecents) / 1200.0);
}

double evaluate(const Dahdsr& envelope,
                double seconds,
                double note_off,
                double release_start_cb) noexcept
{
    const double delay = from_timecents(envelope.delay);
    const double attack = from_timecents(envelope.attack);
    const double hold = from_timecents(envelope.hold);
    const double sustain = std::clamp(static_cast<double>(envelope.sustain), 0.0, cb_silence);

    // Both of these generators measure a fall of the whole floor; the phase that uses them travels
    // only part of it and is scaled to match.
    const double decay = from_timecents(envelope.decay) * (sustain / cb_silence);

    if (note_off >= 0.0 && seconds >= note_off) {
        const double release =
            from_timecents(envelope.release) * ((cb_silence - release_start_cb) / cb_silence);
        if (release <= 0.0) {
            return 0.0;
        }
        const double elapsed = seconds - note_off;
        const double cb =
            release_start_cb + ((elapsed / release) * (cb_silence - release_start_cb));
        return cb >= cb_silence ? 0.0 : gain_of(cb);
    }

    if (seconds < delay) {
        return 0.0;
    }
    const double after_delay = seconds - delay;

    // The attack is the one stage that is linear in gain rather than in centibels.
    if (after_delay < attack) {
        return attack <= 0.0 ? 1.0 : after_delay / attack;
    }
    const double after_attack = after_delay - attack;

    if (after_attack < hold) {
        return 1.0;
    }
    const double after_hold = after_attack - hold;

    if (after_hold < decay) {
        return gain_of((after_hold / decay) * sustain);
    }
    return gain_of(sustain);
}

VolumeFit fit_volume(const SegmentEnvelope& source,
                     int sample_rate,
                     double hold_seconds,
                     double delay_seconds)
{
    VolumeFit fit;
    const auto rate = static_cast<double>(sample_rate);
    const auto targets = source.targets();
    const auto ends = source.segment_ends();

    // How many segments actually move, which is the number that decides representability.
    double previous = 0.0;
    for (int i = 0; i < SegmentEnvelope::segment_count; ++i) {
        if (std::abs(targets[i] - previous) > 1e-9) {
            ++fit.moving_segments;
        }
        previous = targets[i];
    }

    const auto peak_iterator = std::max_element(targets.begin(), targets.end());
    fit.peak = *peak_iterator;
    const auto peak_index = static_cast<int>(std::distance(targets.begin(), peak_iterator));

    if (fit.peak <= 0.0) {
        // Nothing sounds. Leave the envelope instant and silent rather than emitting a shape.
        fit.peak = 1.0;
        fit.envelope.sustain = static_cast<int>(cb_silence);
        return fit;
    }

    const double attack_seconds = static_cast<double>(ends[peak_index]) / rate;

    // A hold is the segments straight after the peak that stay at it.
    int hold_index = peak_index;
    while (hold_index + 1 < SegmentEnvelope::segment_count
           && targets[hold_index + 1] >= fit.peak * 0.995) {
        ++hold_index;
    }
    const double hold_end_seconds = static_cast<double>(ends[hold_index]) / rate;
    const double final_seconds =
        static_cast<double>(ends[SegmentEnvelope::segment_count - 1]) / rate;

    const double sustain_cb = to_centibels(targets[SegmentEnvelope::segment_count - 1] / fit.peak);
    const double decay_seconds = std::max(0.0, final_seconds - hold_end_seconds);
    const double release_seconds = static_cast<double>(source.release_samples()) / rate;

    fit.envelope.delay = to_timecents(delay_seconds);
    fit.envelope.attack = to_timecents(attack_seconds);
    fit.envelope.hold = to_timecents(std::max(0.0, hold_end_seconds - attack_seconds));
    fit.envelope.sustain = static_cast<int>(std::lround(sustain_cb));

    // Unscale both durations into the "fall the whole floor" units the generators hold.
    fit.envelope.decay =
        sustain_cb > 0.0 ? to_timecents(decay_seconds * (cb_silence / sustain_cb)) : min_timecents;
    fit.envelope.release =
        sustain_cb < cb_silence
            ? to_timecents(release_seconds * (cb_silence / (cb_silence - sustain_cb)))
            : min_timecents;

    // ── how badly does it fit ────────────────────────────────────────────────
    //
    // Measured against the engine's own envelope over the held portion plus the release, at the
    // control rate rather than per sample -- the shapes differ by far more than a sample of
    // alignment, so a coarse grid measures the same thing for a hundredth of the work.
    const double note_off = std::max(hold_seconds, final_seconds);
    const double total = note_off + release_seconds;
    const auto steps = static_cast<int>(std::min(20000.0, std::max(64.0, total * 1000.0)));

    SegmentEnvelope reference = source;
    reference.note_off(static_cast<std::int64_t>(note_off * rate));

    // The first two milliseconds are excluded, and not to flatter the fit. The engine's amplitude
    // attack is genuinely instantaneous -- a great many partials have a zero-length first segment
    // and stand at full peak on sample zero -- while SF2's shortest expressible attack is about a
    // millisecond. Including that window makes every such partial report an error equal to its own
    // peak, which measures the format's floor rather than the quality of the fit and drowns out
    // the differences that matter.
    constexpr double attack_floor_seconds = 0.002;

    double sum_squares = 0.0;
    int measured = 0;
    for (int step = 0; step < steps; ++step) {
        const double seconds = (total * step) / steps;
        if (seconds < attack_floor_seconds) {
            continue;
        }
        ++measured;
        const double engine = reference.value_at(static_cast<std::int64_t>(seconds * rate));
        const double fitted =
            evaluate(fit.envelope, seconds, note_off, static_cast<double>(fit.envelope.sustain))
            * fit.peak;
        // Normalised by the peak, so a quiet partial and a loud one are compared on equal terms.
        const double error = std::abs(engine - fitted) / fit.peak;
        fit.worst_error = std::max(fit.worst_error, error);
        sum_squares += error * error;
    }
    fit.rms_error = measured > 0 ? std::sqrt(sum_squares / measured) : 0.0;

    return fit;
}

} // namespace ts::sf2
