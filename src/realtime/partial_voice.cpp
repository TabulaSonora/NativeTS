#include "realtime/partial_voice.hpp"

#include "tabulasonora/note_renderer.hpp"

#include <algorithm>
#include <cmath>

namespace ts {
namespace {

/// Samples per control tick — the grid coefficients refresh on.
constexpr int control_block = NoteRenderer::control_block;

/// Pan units the position may move in one control tick.
constexpr int pan_slew_per_tick = 2;

/// Controller units a send may move in one control tick.
///
/// The engine steps the send's gain word by 8 of 1024 a tick and full scale is 1016/1024, so it
/// takes 127 ticks -- one unit of CC#91 each -- and 1.27 s to cross. Measured directly: `scdec
/// sendramp` on CC#91 0 -> 127 is 127 steps of 8/1024, one every 320 samples, 1260 ms.
constexpr double send_slew_per_tick = 1.0;

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
    hold_samples_ = setup.envelope_hold_samples;
    half_damper_ = setup.half_damper;
    glide_ = setup.glide_milli_semitones;
    glide_step_ = setup.glide_step;
    is_drum_ = setup.is_drum;
    drum_receives_note_off_ = setup.drum_receives_note_off;
    level_gain_ = setup.level_gain;
    pan_ = setup.pan;
    pan_follows_part_ = setup.pan_follows_part;
    random_pan_ = setup.random_pan.value_or(-1);
    pan_seeded_ = false;
    sends_seeded_ = false;

    volume_ramp_.seed(ControlRamp::target_of(setup.volume_word),
                      ControlRamp::volume_rate_word,
                      setup.volume_mask);

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
        native_pitch_ = setup.descriptor.native_milli_semitones();
    }

    pitch_modulation_ = 0.0;
    tremolo_ = 1.0;
    ratio_ = 1.0;
    entry_ratio_ = 1.0;
    slot_ratio_ = 1.0;
    slot_remaining_ = 0;
}

void PartialVoice::note_off(int damper)
{
    // The kit decides, per key. Ignoring note-off on every drum was close enough to right that it
    // stood for a long time -- almost no key answers one -- but it is not the rule, and standing in
    // for it with a fixed ring timer cut every long drum off at the same arbitrary moment. A crash
    // cymbal whose decay the module runs for 4.7 seconds was being killed at 1.8.
    if (is_drum_ && !drum_receives_note_off_) {
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

int PartialVoice::pan_target(int part_pan) const noexcept
{
    // A random pan is resolved once for the strike and is not a moving target, so it never slews.
    if (random_pan_ >= 0) {
        return random_pan_;
    }
    return std::clamp(pan_follows_part_ ? pan_ + (part_pan - PanLaw::centre) : pan_, 0, 127);
}

void PartialVoice::slew_pan(int part_pan) noexcept
{
    const int target = pan_target(part_pan);

    // The first call lands on the target outright: a note struck while a pan sweep is in flight
    // starts where the sweep has got to, it does not set off from the centre to catch up.
    if (!pan_seeded_) {
        pan_position_ = target;
        pan_seeded_ = true;
        return;
    }

    if (sample_ % control_block != 0) {
        return;
    }

    // Two units a tick, and the last one exactly: an error of one moves by one, so the position
    // arrives on the target rather than oscillating either side of it.
    pan_position_ += std::clamp(target - pan_position_, -pan_slew_per_tick, pan_slew_per_tick);
}

std::pair<double, double> PartialVoice::pan_gains() const noexcept
{
    return pan_law_->gains(pan_position_);
}

void PartialVoice::slew_sends(int reverb, int chorus, int delay) noexcept
{
    const std::array<double, 3> targets{static_cast<double>(reverb),
                                        static_cast<double>(chorus),
                                        static_cast<double>(delay)};

    // As with the pan, a voice starts at the level rather than fading its sends up to it.
    if (!sends_seeded_) {
        sends_ = targets;
        sends_seeded_ = true;
        return;
    }

    if (sample_ % control_block != 0) {
        return;
    }

    for (std::size_t i = 0; i < sends_.size(); ++i) {
        sends_[i] +=
            std::clamp(targets[i] - sends_[i], -send_slew_per_tick, send_slew_per_tick);
    }
}

double PartialVoice::choke_gain(std::int64_t since) noexcept
{
    if (since < choke_hold) {
        return 1.0;
    }
    const std::int64_t into = since - choke_hold;
    return into < choke_fade ? 1.0 - (static_cast<double>(into) / choke_fade) : 0.0;
}

void PartialVoice::volume_gains(int word, unsigned mask, std::span<double> gains) noexcept
{
    // Every control tick, whether the target moved or not: `voice_ramp_target_aux` sits in the
    // per-tick voice update, not behind a change test. It costs a truncation of the accumulator's
    // low ten bits each time, which is invisible in the oracle's gain trace either way -- the
    // per-update ratio holds at 0.90403 straight across the tick boundaries a glide crosses -- so
    // this follows the decompile rather than the measurement, which cannot separate them.
    //
    // The same test `render` uses to find a control tick, and it reads the clock before `render`
    // advances it, which is why the two calls are ordered.
    if (sample_ % control_block == 0) {
        volume_ramp_.retarget(ControlRamp::target_of(word), ControlRamp::volume_rate_word, mask);
    }
    volume_ramp_.fill(gains);
}

void PartialVoice::render(std::span<float> destination,
                          double bend_milli_semitones,
                          const ControlMatrix::Modulation& matrix)
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
        control(bend_milli_semitones, matrix, control_tick_ == 0);
        ++control_tick_;
        pitch_ramp_.arm(entry_ratio_, ratio_);
        slot_remaining_ = 0;
    } else {
        // Bend moves on the block grid even between control ticks.
        ratio_ = ratio(bend_milli_semitones);
    }

    for (std::size_t i = 0; i < destination.size(); ++i) {
        if (slot_remaining_ <= 0) {
            // A settled ramp hands back the exact ratio rather than the table's decode of it, so a
            // steady note renders as it did before this existed; only the glide is new.
            slot_ratio_ = pitch_ramp_.is_active()
                              ? static_cast<double>(pitch_ramp_.next_slot()) / 65536.0
                              : ratio_;
            slot_remaining_ = PitchRamp::samples_per_slot;
        }
        --slot_remaining_;

        auto value = static_cast<double>(reader_.next(slot_ratio_));

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

    // No timer bounds a voice's life, because the module has none: it ends when its own
    // amplitude envelope reaches silence, when a one-shot sample runs out, or when a choke
    // finishes. A drum whose envelope sustains is by the kit data always either a one-shot --
    // the sample ends it -- or a looping key marked Rx Note Off, which the file ends with a
    // note-off, exactly like the snare rolls the old fixed ring was cutting at 1.8 s. A file
    // that holds such a key forever gets a voice that sounds forever, which is what the
    // hardware does too.
    if (reader_.finished() || !amplitude_ || amplitude_->is_finished(envelope_sample(sample_))
        || (choke_at_ >= 0 && sample_ - choke_at_ >= choke_hold + choke_fade)) {
        kill();
    }
}

void PartialVoice::control(double bend_milli_semitones,
                           const ControlMatrix::Modulation& matrix,
                           bool first)
{
    const bool is_released = note_off_ >= 0 && sample_ >= note_off_;

    // The first control tick carries no LFO: the LFO object is created after that tick's update, so
    // nothing has been applied yet. Aligning to that is what makes the modulation line up with the
    // engine's own trace.
    double lfo_pitch = 0.0;
    double lfo_tvf = 0.0;
    double lfo_tva = 0.0;

    if (!first) {
        // Eight of the matrix's eleven destinations are the two LFOs' rate and three depths. The
        // rate is consumed by the tick, since it moves the phase; the depths are consumed reading
        // the value out, since they scale the waveform the phase produced.
        if (lfo1_) {
            lfo1_->tick(matrix.lfo1_rate);
        }
        if (lfo2_) {
            lfo2_->tick(matrix.lfo2_rate);
        }

        if (lfo1_) {
            lfo_pitch += lfo1_->value(LfoDestination::pitch, matrix.lfo1_pitch);
            lfo_tvf += lfo1_->value(LfoDestination::tvf, matrix.lfo1_tvf);
            lfo_tva += lfo1_->value(LfoDestination::tva, matrix.lfo1_tva);
        }
        if (lfo2_) {
            lfo_pitch += lfo2_->value(LfoDestination::pitch, matrix.lfo2_pitch);
            lfo_tvf += lfo2_->value(LfoDestination::tvf, matrix.lfo2_tvf);
            lfo_tva += lfo2_->value(LfoDestination::tva, matrix.lfo2_tva);
        }
    }

    // voice_pitch_block_init records the pitch entering the block at voice+0xb8, before the
    // envelope steps, and the pitch leaving it at +0xbc. The ramp glides between the two. Taking
    // the entry value here is what makes the envelope's start level audible at all: it is consumed
    // by the first tick, so a renderer that only ever applies post-tick values never sees it.
    entry_ratio_ = ratio_with((pitch_envelope_ ? pitch_envelope_->level() : 0.0) + lfo_pitch
                                  + glide_,
                              bend_milli_semitones);

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
    //
    // The matrix's amplitude destination is summed with the two LFOs' before that clamp, not after
    // and not separately: the engine adds all three and clamps the total once, so a part already
    // driven to the rail by tremolo cannot be pushed past it by a controller.
    tremolo_ = 1.0
               + (std::clamp(lfo_tva + matrix.amplitude,
                             -static_cast<double>(0x7F00),
                             static_cast<double>(0x7F00))
                  / static_cast<double>(0x7F00));

    if (tap_ != FilterTap::bypass && cutoff_) {
        // Averaged over the block the coefficients are about to serve, not sampled at its start.
        // The envelope can cross several segments inside one 10 ms tick, and taking a single point
        // instead costs about 1.7% of peak on a piano attack -- the one place where the two render
        // paths measurably parted company.
        // The matrix's cutoff destination joins the sum the engine clamps, alongside both LFOs'
        // filter modulation -- `tvf_cutoff_add_lfo` adds every term and clamps once at the end,
        // rather than clamping the running cutoff and then adding to it.
        double total = 0.0;
        for (int n = 0; n < control_block; ++n) {
            total += std::clamp(cutoff_base_ + cutoff_offset_
                                    + cutoff_->value_at(envelope_sample(sample_ + n)) + lfo_tvf
                                    + matrix.tvf_cutoff,
                                0.0,
                                static_cast<double>(0x7FFF));
        }

        const int units = tvf_->cutoff_units(total / control_block, resonance_byte_);
        const auto coefficients = tvf_->coefficients(units, resonance_byte_, filter_type_);
        frequency_ = coefficients.frequency;
        damping_ = coefficients.damping;
    }
}

double PartialVoice::ratio(double bend_milli_semitones) const
{
    return ratio_with(pitch_modulation_, bend_milli_semitones);
}

double PartialVoice::ratio_with(double modulation, double bend_milli_semitones) const
{
    if (is_drum_) {
        // Drums take a different pitch route: the note does not transpose the sample, so there is
        // no key-follow and no absolute-pitch accumulator to clamp.
        return drum_base_ratio_ * std::pow(2.0, modulation / 12000.0);
    }

    const double pitch = base_pitch_ + modulation + bend_milli_semitones;
    return std::pow(2.0, (PitchChain::clamp(pitch) - native_pitch_) / 12000.0);
}

} // namespace ts
