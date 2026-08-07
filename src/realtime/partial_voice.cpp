#include "realtime/partial_voice.hpp"

#include "tabulasonora/note_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
namespace {

/// Samples per control tick — the grid coefficients refresh on.
constexpr int control_block = NoteRenderer::control_block;

/// Diagnostic probes for a single voice, selected by `TS_TVF_TRACE=<channel>:<note>`.
///
/// Scoped to one channel *and* one MIDI note, and reported against the voice's own sample offset
/// rather than render time. A voice does not know the absolute position of the render, and a
/// file-wide maximum answers the wrong question: it reports the loudest thing anywhere in 290
/// seconds when what is wanted is what happened during one second of one note. Several voices on a
/// channel also interleave their calls, so a probe that is not note-scoped differences one voice's
/// output against another's and reports a step neither of them took.
struct Watch {
    int channel = -1;
    int note = -1;
};

[[nodiscard]] Watch watched() noexcept
{
    static const Watch instance = [] {
        Watch w;
        const char* spec = std::getenv("TS_TVF_TRACE");
        if (spec == nullptr) {
            return w;
        }
        w.channel = std::atoi(spec);
        if (const char* colon = std::strchr(spec, ':')) {
            w.note = std::atoi(colon + 1);
        }
        return w;
    }();
    return instance;
}

/// The pitch chain, term by term, at the tick where the ratio is highest.
///
/// `ratio_with` sums `base_pitch_ + envelope + lfo + glide + bend`, clamps once, and takes the
/// exponential against the wave's native pitch. Every term is in milli-semitones, so 12000 is an
/// octave and the ratio is what the resampler is finally driven at. Recording all of them at the
/// worst tick says which one moved rather than that something did.
void pitch_trace(int channel,
                 int note,
                 std::int64_t at,
                 double base,
                 double envelope,
                 double lfo,
                 double glide,
                 double bend,
                 double native,
                 double ratio) noexcept
{
    const Watch w = watched();
    if (w.channel < 0 || channel != w.channel || (w.note >= 0 && note != w.note)) {
        return;
    }
    static double peak = 0.0;
    static std::int64_t peak_at = 0;
    static double b = 0.0;
    static double e = 0.0;
    static double l = 0.0;
    static double g = 0.0;
    static double bd = 0.0;
    static double nat = 0.0;
    static double first_ratio = -1.0;
    if (first_ratio < 0.0) {
        first_ratio = ratio;
    }
    if (ratio > peak) {
        peak = ratio;
        peak_at = at;
        b = base;
        e = envelope;
        l = lfo;
        g = glide;
        bd = bend;
        nat = native;
    }
    static const struct Report {
        ~Report()
        {
            std::fprintf(stderr,
                         "pitch:  ratio %.4f -> %.4f at %.3f s | base %.0f  env %+.0f  lfo %+.0f  "
                         "glide %+.0f  bend %+.0f | native %.0f  sum %.0f\n",
                         first_ratio, peak, static_cast<double>(peak_at) / 32000.0, b, e, l, g, bd,
                         nat, b + e + l + g + bd);
        }
    } report;
}

/// Worst filter stability margin, and where in the note it fell.
///
/// The bound is the recurrence's own, derived rather than assumed: substituting the three lines of
/// `StateVariableFilter::process` gives the characteristic polynomial
/// `z^2 + (f^2 + f*q - 2)*z + (1 - f*q)`, and Jury's criterion on that is `f*q < 2` together with
/// `f^2 + 2*f*q < 4`. `4 - (f^2 + 2*f*q)` is the margin, and a negative one puts a pole outside the
/// unit circle -- heard as full-amplitude oscillation near Nyquist rather than as a filtered note.
///
/// The looser `f < 2 - q` usually quoted for this topology is not the stability boundary and
/// reports breaches on ordinary material; it was tried first here and had to be discarded.
void tvf_trace(int channel, int note, std::int64_t at, double f, double q) noexcept
{
    const Watch w = watched();
    if (w.channel < 0 || channel != w.channel || (w.note >= 0 && note != w.note)) {
        return;
    }
    static double worst = 4.0;
    static std::int64_t worst_at = 0;
    static double worst_f = 0.0;
    static double worst_q = 0.0;
    static std::int64_t breaches = 0;
    const double margin = 4.0 - ((f * f) + (2.0 * f * q));
    if (margin < worst) {
        worst = margin;
        worst_at = at;
        worst_f = f;
        worst_q = q;
    }
    if (margin <= 0.0) {
        ++breaches;
    }
    static const struct Report {
        ~Report()
        {
            std::fprintf(stderr,
                         "tvf:    worst margin %.4f (f=%.4f q=%.4f) at %.3f s into the note; "
                         "%lld breaches\n",
                         worst, worst_f, worst_q, static_cast<double>(worst_at) / 32000.0,
                         static_cast<long long>(breaches));
        }
    } report;
}

/// The resampler's own output, before the filter sees it.
///
/// Places the burst on one side of the filter or the other: a step this large in the *reader's*
/// output is aliasing or a bad read, and nothing downstream would be the cause.
void reader_trace(int channel, int note, std::int64_t at, double raw, double ratio) noexcept
{
    const Watch w = watched();
    if (w.channel < 0 || channel != w.channel || (w.note >= 0 && note != w.note)) {
        return;
    }
    static double previous = 0.0;
    static double worst_step = 0.0;
    static std::int64_t worst_at = 0;
    static double worst_ratio = 0.0;
    static double max_ratio = 0.0;
    static std::int64_t max_ratio_at = 0;
    const double step = std::abs(raw - previous);
    previous = raw;
    if (step > worst_step) {
        worst_step = step;
        worst_at = at;
        worst_ratio = ratio;
    }
    if (ratio > max_ratio) {
        max_ratio = ratio;
        max_ratio_at = at;
    }
    static const struct Report {
        ~Report()
        {
            std::fprintf(stderr,
                         "reader: worst step %.4f at %.3f s into the note (ratio %.4f); "
                         "peak ratio %.4f at %.3f s\n",
                         worst_step, static_cast<double>(worst_at) / 32000.0, worst_ratio,
                         max_ratio, static_cast<double>(max_ratio_at) / 32000.0);
        }
    } report;
}

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
    send_seeded_ = false;

    volume_ramp_.seed(ControlRamp::target_of(setup.volume_word),
                      ControlRamp::volume_rate_word,
                      setup.volume_mask);

    reader_.start(*setup.wave);
    filter_.reset();
    // Along with the filter state: a slot that has just been stolen must not glide from the
    // coefficient the previous note ended on.
    frequency_ramp_ = CoefficientRamp{};
    damping_ramp_ = DampingRamp{};

    amplitude_ = std::move(setup.amplitude);
    cutoff_ = std::move(setup.cutoff);
    cutoff_base_ = setup.cutoff_base;
    pitch_envelope_ = std::move(setup.pitch_envelope);
    lfo1_ = std::move(setup.lfo1);
    lfo1_depths_ = setup.lfo1_depths;
    lfo2_ = std::move(setup.lfo2);

    partial_resonance_ = setup.partial.resonance();
    resonance_byte_ = TvfChain::resonance_byte_of(partial_resonance_, setup.part_resonance);
    filter_type_ = setup.partial.filter_type();
    tap_ = tvf_->tap(filter_type_);

    if (setup.is_drum) {
        drum_base_ratio_ = setup.drum_base_ratio;
    } else {
        base_pitch_ = setup.base_pitch;
        native_pitch_ = setup.descriptor.native_milli_semitones();
    }

    // A note starts at `voice+0x1fc` and moves to `voice+0x200` once its decoder passes the loop
    // point -- see `WaveDescriptor::second_fine_tune`. One offset serves both pitch routes, because
    // the drum's native is baked into `drum_base_ratio_` against the same word. Zero for the four
    // records in five whose term is neutral, which costs those voices nothing.
    second_fine_offset_ = setup.descriptor.adopted_milli_semitones()
                          - setup.descriptor.native_milli_semitones();
    second_fine_shift_ = 0.0;

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
        //
        // **Ten partials in the whole table are these**, and Roland's own naming gives them away --
        // the `.o` suffix is "off": `Harpsi.o` (tones 44, 1647), `Clav.o` (53), `Organ o` (99),
        // `Nylon Gt.o` (163, 1499, 1669), plus `MandolinTrem`, `Aqua` and `Biwa 3`. Find them by
        // scanning the tone table for a partial whose block `+0x00` is `0xff`.
        //
        // **No piano has one**, which answers a question worth not re-asking: a harpsichord's
        // note-off click is the jack falling back, real pianos thump when the dampers return, and
        // the obvious guess is that Roland sampled both. They did not sample the piano's. The
        // instruments that carry a key-off layer are the plucked and stopped ones.
        //
        // Nor is the layer a second partial of the capital tone. `Harpsichord` at bank 0 has one
        // partial and no release slot; `Harpsi.o` is a separate two-partial tone at bank 24, slot 0
        // being the key-off layer and slot 1 an ordinary partial, and a sequencer selects it rather
        // than getting it for free. Measured against the module on that tone: level agrees to
        // 0.01 dB while the note is held and 0.3 dB after the note-off.
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

void PartialVoice::slew_reverb_send(int level) noexcept
{
    // Only the reverb send is a per-voice slot. Chorus and delay are one matrix coefficient a part
    // and are slewed on `Part` instead.
    const auto target = static_cast<double>(level);

    // As with the pan, a voice starts at the level rather than fading its send up to it.
    if (!send_seeded_) {
        reverb_send_ = target;
        send_seeded_ = true;
        return;
    }

    if (sample_ % control_block != 0) {
        return;
    }

    reverb_send_ += std::clamp(target - reverb_send_, -send_slew_per_tick, send_slew_per_tick);
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
            // Through the table whether the ramp is gliding or settled, because the module has no
            // other route to a playback rate: `ramp_env_step_eval` writes an increment out of
            // `g_ramp_exp_tbl` every eight samples for the whole life of every voice.
            //
            // The table is not a true exponential. Measured against one it drifts linearly to
            // **4.66 cents flat** across an octave and snaps back at the boundary, so the module's
            // sounding pitch carries a sawtooth of that size against equal temperament. Handing a
            // settled note the exact ratio instead spends that sawtooth as a *disagreement*: two
            // parts whose pitch words sit at different points of the octave are put a different
            // distance out, and the pair beats against each other at a rate the module does not
            // have. That is what SC-55 mode sounded like on `earthbnd.mid` -- the clarinet against
            // the square lead -- and it is why the shortcut this replaces cost more than it saved.
            slot_ratio_ = static_cast<double>(pitch_ramp_.next_slot()) / 65536.0;
            slot_remaining_ = PitchRamp::samples_per_slot;
        }
        --slot_remaining_;

        auto value = static_cast<double>(reader_.next(slot_ratio_));
        reader_trace(channel_, note_, sample_, value, slot_ratio_);

        if (tap_ != FilterTap::bypass) {
            // Stepped per sample rather than held for the block: the engine glides its frequency
            // coefficient, and at high resonance a per-block step relocates a ringing filter's pole
            // and re-excites it once every control tick.
            //
            // The two ramps are separate objects, so pulling them into locals fixes an order the
            // language left unspecified without changing what either one yields.
            const double f = frequency_ramp_.step();
            const double q = damping_ramp_.step();
            tvf_trace(channel_, note_, sample_, f, q);
            value = filter_.process(value, f, q, tap_);
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
            // Once per tick for the node, not once per partial pointing at it -- `sample_` is the
            // note's age, so siblings all present the same stamp and only the first advances it.
            lfo1_->tick_at(sample_, matrix.lfo1_rate);
        }
        if (lfo2_) {
            lfo2_->tick(matrix.lfo2_rate);
        }

        if (lfo1_) {
            // Shared phase, this partial's own depths.
            lfo_pitch += lfo1_->value_at_depth(LfoDestination::pitch, lfo1_depths_.pitch_depth,
                                               matrix.lfo1_pitch);
            lfo_tvf += lfo1_->value_at_depth(LfoDestination::tvf, lfo1_depths_.tvf_depth,
                                             matrix.lfo1_tvf);
            lfo_tva += lfo1_->value_at_depth(LfoDestination::tva, lfo1_depths_.tva_depth,
                                             matrix.lfo1_tva);
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

    // The second fine tune, adopted between the block's two endpoints so the ramp glides into it.
    //
    // `voices_control_update` copies `voice+0x200` over `voice+0x1fc` on the first tick after the
    // decoder has passed the loop point, and the pitch ramp is *retargeted* rather than reassigned:
    // traced on the module, the target moves and the current value chases it over two slots. Doing
    // it here -- after `entry_ratio_` was formed from the old tuning and before `ratio_` is formed
    // from the new -- is that retarget, because `render` arms the ramp between the two. Assigning
    // it underneath the ramp instead is what made an earlier attempt at this fail.
    if (second_fine_shift_ != second_fine_offset_ && reader_.passed_loop_start()) {
        second_fine_shift_ = second_fine_offset_;
    }

    pitch_modulation_ = envelope + lfo_pitch + glide_;
    ratio_ = ratio(bend_milli_semitones);
    pitch_trace(channel_, note_, sample_, base_pitch_, envelope, lfo_pitch, glide_,
                bend_milli_semitones, native_pitch_, ratio_);

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

        // The ramp walks the accumulator the engine walks, not the decoded coefficient: the decode
        // drops three bits, so gliding the double between the same two endpoints would trace a
        // different curve.
        //
        // `coefficients` may have clamped `f` down to the stability ceiling `q` selects. Where it
        // did, the ramp is aimed at the clamped value instead; the ramp only ever sits between its
        // own start and its target, so a target under the ceiling keeps every step under it too.
        const int raw = tvf_->frequency_accumulator(units);
        const int target =
            coefficients.frequency < CoefficientRamp::decode(raw)
                ? static_cast<int>(coefficients.frequency * 16384.0) << CoefficientRamp::decode_shift
                : raw;

        const int damping = DampingRamp::encode(coefficients.damping);

        if (frequency_ramp_.is_seeded()) {
            frequency_ramp_.retarget(target);
            damping_ramp_.retarget(damping);
        } else {
            // A voice's first block starts *at* its coefficients rather than gliding up from
            // whatever the previous occupant of this slot left behind.
            frequency_ramp_.seed(target);
            damping_ramp_.seed(damping);
        }
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
        return drum_base_ratio_ * std::pow(2.0, (modulation - second_fine_shift_) / 12000.0);
    }

    const double pitch = base_pitch_ + modulation + bend_milli_semitones;

    // Two clamps, not one, and they bound different quantities. `PitchChain::clamp` bounds the
    // absolute accumulator at 0x1f018; this one bounds the *increment* the resampler is finally
    // driven by, which is what `voice_pitch_block_init` limits:
    //
    //     increment = 0x38000 + relative_milli_semitones * 512/375, clamped to 0x3fffe
    //
    // and `(0x3fffe - 0x38000) * 375/512` is 23999.5, so the module cannot play a wave faster than
    // four times its native rate however far the pitch is driven. Without this a partial near the
    // top of its range takes the whole of a control-matrix pitch assignment and runs past it --
    // `bigben` reaches 4.80x on a Kalimba wave, and a 4-tap kernel with no band-limiting folds
    // everything above a quarter of the rate back down as it goes.
    //
    // The wave's second fine tune is inside the clamped quantity rather than outside it: it is part
    // of what the pitch chain hands the resampler, so it is part of what becomes the increment
    // word. Subtracting it afterwards would let a voice sitting on the ceiling be pushed back under
    // it by a term the module has already folded in.
    static constexpr double max_increment_milli_semitones = 24000.0;
    const double relative = PitchChain::clamp(pitch) - native_pitch_ - second_fine_shift_;
    return std::pow(2.0, std::min(relative, max_increment_milli_semitones) / 12000.0);
}

} // namespace ts
