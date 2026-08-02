#include "realtime/partial_voice.hpp"

#include "tabulasonora/note_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace ts {
namespace {

/// Samples per control tick — the grid coefficients refresh on.
constexpr int control_block = NoteRenderer::control_block;

} // namespace

void PartialVoice::start(VoiceSetup&& setup)
{
    channel_ = setup.channel;
    note_ = setup.note;
    mute_group_ = setup.mute_group;
    finished_ = false;

    sample_ = 0;
    note_off_ = -1;
    choke_at_ = -1;
    control_tick_ = 0;
    auto_release_ = setup.auto_release_samples;
    hold_samples_ = setup.envelope_hold_samples;
    half_damper_ = setup.half_damper;
    glide_ = setup.glide_milli_semitones;
    glide_step_ = setup.glide_step;
    is_drum_ = setup.is_drum;
    level_gain_ = setup.level_gain;
    pan_ = setup.pan;
    pan_follows_part_ = setup.pan_follows_part;
    random_pan_ = setup.random_pan.value_or(-1);

    reader_.start(*setup.wave);
    filter_.reset();

    amplitude_ = std::move(setup.amplitude);
    cutoff_ = std::move(setup.cutoff);
    cutoff_base_ = setup.cutoff_base;
    pitch_envelope_ = std::move(setup.pitch_envelope);
    lfo1_ = std::move(setup.lfo1);
    lfo2_ = std::move(setup.lfo2);

    resonance_byte_ = TvfChain::resonance_byte(setup.partial);
    filter_type_ = setup.partial.filter_type();
    tap_ = tvf_->tap(filter_type_);

    if (setup.is_drum) {
        drum_base_ratio_ = setup.drum_base_ratio;
    } else {
        base_pitch_ = setup.base_pitch;
        native_pitch_ = (setup.descriptor.root_key * 1000.0) + 1024.0 - setup.descriptor.fine_tune;
    }

    pitch_modulation_ = 0.0;
    tremolo_ = 1.0;
    ratio_ = 1.0;
}

void PartialVoice::note_off(int damper)
{
    if (is_drum_) {
        return;
    }

    if (hold_samples_ == EnvelopeMachine::hold_forever) {
        // A key-off layer fires here rather than releasing: the note-off is consumed arming it.
        hold_samples_ = SegmentEnvelope::defer_to_control_tick(sample_, control_block);
        return;
    }

    if (sample_ < hold_samples_) {
        // A voice whose delayed start has not fired yet is killed without ever sounding.
        kill();
        return;
    }

    // On a half-damper tone (the pianos) the raw pedal value scales the release rates. Every other
    // tone quantises the pedal, so the value here is zero by the time a release can engage.
    release(half_damper_ ? std::clamp(damper, 0, 0x3F) : 0);
}

void PartialVoice::release(int damper)
{
    if (note_off_ >= 0) {
        return;
    }

    // Deferred to the tick the engine would act on, so the pitch envelope -- which reads this flag,
    // and only from the control tick -- releases on the same one as the amplitude and cutoff. The
    // envelopes themselves run on held time, so a delayed start shifts their note-off with it.
    note_off_ = SegmentEnvelope::defer_to_control_tick(sample_, control_block);

    if (pitch_envelope_) {
        pitch_envelope_->set_release_damper(damper);
    }
    if (amplitude_) {
        amplitude_->note_off(envelope_sample(sample_), damper);
    }
    if (cutoff_) {
        cutoff_->note_off(envelope_sample(sample_), damper);
    }
}

void PartialVoice::kill() noexcept
{
    finished_ = true;
    reader_.stop();
    amplitude_.reset();
    cutoff_.reset();
    pitch_envelope_.reset();
    lfo1_.reset();
    lfo2_.reset();
}

std::pair<double, double> PartialVoice::pan_gains(int part_pan) const noexcept
{
    if (random_pan_ >= 0) {
        return pan_law_->gains(random_pan_);
    }
    return pan_law_->gains(pan_follows_part_ ? pan_ + (part_pan - PanLaw::centre) : pan_);
}

double PartialVoice::choke_gain(std::int64_t since) noexcept
{
    if (since < choke_hold) {
        return 1.0;
    }
    const std::int64_t into = since - choke_hold;
    return into < choke_fade ? 1.0 - (static_cast<double>(into) / choke_fade) : 0.0;
}

void PartialVoice::render(std::span<float> destination,
                          double bend_milli_semitones,
                          double mod_wheel_depth)
{
    if (finished_) {
        std::fill(destination.begin(), destination.end(), 0.0F);
        return;
    }

    if (sample_ < hold_samples_) {
        // Armed: the engine renders nothing for a held voice. The wave's read position and every
        // control value stay frozen, so the whole voice is simply time-shifted by the hold.
        std::fill(destination.begin(), destination.end(), 0.0F);
        sample_ += static_cast<std::int64_t>(destination.size());
        return;
    }

    if (sample_ % control_block == 0) {
        control(bend_milli_semitones, mod_wheel_depth, control_tick_ == 0);
        ++control_tick_;
    } else {
        // Bend moves on the block grid even between control ticks.
        ratio_ = ratio(bend_milli_semitones);
    }

    for (std::size_t i = 0; i < destination.size(); ++i) {
        auto value = static_cast<double>(reader_.next(ratio_));

        if (tap_ != FilterTap::bypass) {
            value = filter_.process(value, frequency_, damping_, tap_);
        }

        double gain =
            (amplitude_ ? amplitude_->value_at(envelope_sample(sample_)) : 0.0) * tremolo_;

        if (choke_at_ >= 0) {
            gain *= choke_gain(sample_ - choke_at_);
        }

        destination[i] = static_cast<float>(value * gain);
        ++sample_;
    }

    if (auto_release_ >= 0 && sample_ >= auto_release_) {
        // A drum rings for a fixed time and then takes its release. The tone's own envelope has
        // usually done the decay long before; this only bounds the voice's life, since a drum never
        // sees a note-off of its own.
        release();
    }

    if (reader_.finished() || !amplitude_ || amplitude_->is_finished(envelope_sample(sample_))
        || (choke_at_ >= 0 && sample_ - choke_at_ >= choke_hold + choke_fade)) {
        kill();
    }
}

void PartialVoice::control(double bend_milli_semitones, double mod_wheel_depth, bool first)
{
    const bool is_released = note_off_ >= 0 && sample_ >= note_off_;

    // The first control tick carries no LFO: the LFO object is created after that tick's update, so
    // nothing has been applied yet. Aligning to that is what makes the modulation line up with the
    // engine's own trace.
    double lfo_pitch = 0.0;
    double lfo_tvf = 0.0;
    double lfo_tva = 0.0;

    if (!first) {
        if (lfo1_) {
            lfo1_->tick();
        }
        if (lfo2_) {
            lfo2_->tick();
        }

        if (lfo1_) {
            // Only LFO1 takes the mod wheel; LFO2 is unaffected.
            lfo_pitch += lfo1_->pitch_value(mod_wheel_depth);
            lfo_tvf += lfo1_->value(LfoDestination::tvf);
            lfo_tva += lfo1_->value(LfoDestination::tva);
        }
        if (lfo2_) {
            lfo_pitch += lfo2_->value(LfoDestination::pitch);
            lfo_tvf += lfo2_->value(LfoDestination::tvf);
            lfo_tva += lfo2_->value(LfoDestination::tva);
        }
    }

    const double envelope = pitch_envelope_ ? pitch_envelope_->tick(is_released) : 0.0;

    // Portamento: a fixed number of milli-semitones per control tick, straight toward zero, so the
    // glide is linear in pitch rather than in frequency. It is summed into the pitch inside the
    // same clamp as everything else, which is why it lives here and not in the base pitch.
    if (glide_ != 0.0) {
        glide_ = glide_ < 0.0 ? std::min(0.0, glide_ + glide_step_)
                              : std::max(0.0, glide_ - glide_step_);
    }

    pitch_modulation_ = envelope + lfo_pitch + glide_;
    ratio_ = ratio(bend_milli_semitones);

    // Amplitude modulation folds in as a fraction of 0x7f00, clamped first.
    tremolo_ = 1.0
               + (std::clamp(lfo_tva, -static_cast<double>(0x7F00), static_cast<double>(0x7F00))
                  / static_cast<double>(0x7F00));

    if (tap_ != FilterTap::bypass && cutoff_) {
        // Averaged over the block the coefficients are about to serve, not sampled at its start.
        // The envelope can cross several segments inside one 10 ms tick, and taking a single point
        // instead costs about 1.7% of peak on a piano attack -- the one place where the two render
        // paths measurably parted company.
        double total = 0.0;
        for (int n = 0; n < control_block; ++n) {
            total +=
                std::clamp(cutoff_base_ + cutoff_->value_at(envelope_sample(sample_ + n)) + lfo_tvf,
                           0.0,
                           static_cast<double>(0x7FFF));
        }

        const int units = tvf_->cutoff_units(total / control_block, resonance_byte_);
        frequency_ = tvf_->frequency_coefficient(units);
        damping_ = tvf_->damping_coefficient(units, resonance_byte_, filter_type_);
    }
}

double PartialVoice::ratio(double bend_milli_semitones) const
{
    if (is_drum_) {
        // Drums take a different pitch route: the note does not transpose the sample, so there is
        // no key-follow and no absolute-pitch accumulator to clamp.
        return drum_base_ratio_ * std::pow(2.0, pitch_modulation_ / 12000.0);
    }

    const double pitch = base_pitch_ + pitch_modulation_ + bend_milli_semitones;
    return std::pow(2.0, (PitchChain::clamp(pitch) - native_pitch_) / 12000.0);
}

} // namespace ts
