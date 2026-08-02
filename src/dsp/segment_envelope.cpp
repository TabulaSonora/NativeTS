#include "tabulasonora/segment_envelope.hpp"

#include <algorithm>
#include <stdexcept>

namespace ts {

SegmentEnvelope::SegmentEnvelope(const EnvelopeMachine& machine,
                                 std::span<const double> targets,
                                 std::span<const double> segment_samples,
                                 std::span<const bool> linear,
                                 double release_target,
                                 double release_samples,
                                 bool release_linear,
                                 double after_release,
                                 std::int64_t control_tick_samples)
    : machine_(&machine),
      release_target_(release_target),
      release_linear_(release_linear),
      release_span_(release_samples),
      release_samples_(std::max<std::int64_t>(1, static_cast<std::int64_t>(release_samples))),
      after_release_(after_release),
      control_tick_(std::max<std::int64_t>(1, control_tick_samples))
{
    if (targets.size() != segment_count || segment_samples.size() != segment_count
        || linear.size() != segment_count) {
        throw std::invalid_argument("An envelope needs exactly four segments.");
    }

    // Boundaries are accumulated in samples rather than per segment, so a run of short segments
    // cannot drift against the position a sample index falls at.
    double elapsed = 0.0;
    for (std::size_t i = 0; i < segment_count; ++i) {
        targets_[i] = targets[i];
        linear_[i] = linear[i];
        span_[i] = segment_samples[i];
        from_[i] = static_cast<std::int64_t>(elapsed);
        elapsed += segment_samples[i];
        to_[i] = static_cast<std::int64_t>(elapsed);
    }
}

void SegmentEnvelope::note_off(std::int64_t sample, int damper)
{
    if (note_off_ >= 0) {
        return;
    }

    if (damper > 0) {
        const double scale = 65536.0 / (0xFFFF - (std::min(damper, 0x3F) << 9));
        release_span_ *= scale;
        release_samples_ = std::max<std::int64_t>(1, static_cast<std::int64_t>(release_span_));
    }

    const std::int64_t deferred = defer_to_control_tick(sample, control_tick_);

    at_note_off_ = held(std::max<std::int64_t>(0, deferred - 1));
    note_off_ = deferred;
}

std::int64_t SegmentEnvelope::defer_to_control_tick(std::int64_t sample,
                                                    std::int64_t control_tick_samples) noexcept
{
    const std::int64_t tick = std::max<std::int64_t>(1, control_tick_samples);
    return (std::max<std::int64_t>(0, sample) / tick * tick) + tick;
}

double SegmentEnvelope::value_at(std::int64_t sample) const noexcept
{
    if (note_off_ < 0 || sample < note_off_) {
        return held(sample);
    }

    const std::int64_t n = sample - note_off_;
    if (n >= release_samples_) {
        return after_release_;
    }

    const double position = release_span_ > 0 ? static_cast<double>(n) / release_span_ : 1.0;
    return machine_->segment_curve(position, at_note_off_, release_target_, release_linear_);
}

double SegmentEnvelope::held(std::int64_t sample) const noexcept
{
    double previous = 0.0;

    for (std::size_t i = 0; i < segment_count; ++i) {
        if (to_[i] > from_[i] && sample < to_[i]) {
            return machine_->segment_curve(static_cast<double>(sample - from_[i]) / span_[i],
                                           previous,
                                           targets_[i],
                                           linear_[i]);
        }
        previous = targets_[i];
    }

    // Every segment has run: hold the last target until note-off.
    return previous;
}

} // namespace ts
