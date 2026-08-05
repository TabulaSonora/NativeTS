#pragma once

#include "realtime/wave_reader.hpp"
#include "tabulasonora/control_matrix.hpp"
#include "tabulasonora/control_ramp.hpp"
#include "tabulasonora/interpolator.hpp"
#include "tabulasonora/lfo_engine.hpp"
#include "tabulasonora/partial_parameters.hpp"
#include "tabulasonora/pitch_chain.hpp"
#include "tabulasonora/pitch_ramp.hpp"
#include "tabulasonora/segment_envelope.hpp"
#include "tabulasonora/state_variable_filter.hpp"
#include "tabulasonora/tvf_chain.hpp"
#include "tabulasonora/wave_descriptor.hpp"

#include <array>
#include <optional>
#include <span>
#include <utility>

namespace ts {

/// Everything a voice latches at note-on.
///
/// The envelopes and runners are moved in rather than referenced: a voice owns its own trajectory
/// for the life of the note, and the pool recycles the voice object, not these.
struct VoiceSetup {
    int channel = 0;
    int note = 0;
    const DecodedWave* wave = nullptr;
    PartialParameters partial;
    WaveDescriptor descriptor;

    std::optional<SegmentEnvelope> amplitude;
    std::optional<SegmentEnvelope> cutoff;
    int cutoff_base = 0;

    std::optional<PitchEnvelopeRunner> pitch_envelope;
    std::optional<LfoRunner> lfo1;
    std::optional<LfoRunner> lfo2;

    /// Absolute base pitch in milli-semitones, for a melodic voice.
    double base_pitch = 0.0;
    /// Playback ratio a drum starts from; drums have no absolute-pitch accumulator.
    double drum_base_ratio = 1.0;
    bool is_drum = false;
    /// Whether this drum key answers a note-off at all — the kit's `Rx.Note Off` bit.
    ///
    /// Almost no key does, and that is the point: a struck sound's envelope is its whole story, so
    /// a drum normally ignores note-off and rings for as long as its own decay says. The exceptions
    /// are the sounds you have to be able to stop — the snare roll, applause, most of the SFX kit —
    /// and `DrumKitTable` reads which from the kit record rather than guessing.
    bool drum_receives_note_off = false;

    int pan = PanLaw::centre;
    bool pan_follows_part = false;
    /// A resolved random pan for this strike, or nothing to use `pan`.
    std::optional<int> random_pan;
    /// The kit level, folded in downstream of the voice gain.
    double level_gain = 1.0;
    /// The drum mute group, or zero.
    int mute_group = 0;

    /// The part's volume word as the voice starts, and the ZOH mask its ramp runs on.
    ///
    /// Seeded rather than ramped to: `tvf_env_prep` writes the same value into the ramp's source and
    /// target slots, so a note struck part-way through a fade begins at the level already reached.
    int volume_word = 0;
    unsigned volume_mask = 0;

    /// Sample at which a voice is force-released, or -1 to let its envelope decide.
    ///
    /// Samples the envelope machine stays held at its note-on state.
    std::int64_t envelope_hold_samples = 0;
    /// Whether this tone responds to a half-pressed damper.
    bool half_damper = false;

    /// Portamento offset in milli-semitones, decaying to zero as the glide completes.
    double glide_milli_semitones = 0.0;
    /// Milli-semitones the glide closes per control tick.
    int glide_step = 0;
};

/// One sounding partial, rendered a block at a time.
///
/// This is the engine's unit of polyphony: a note consumes one of these per sounding partial, so a
/// two-partial patch halves the effective note count. Everything a partial needs to keep between
/// blocks lives here — the read position, the filter state, the LFO accumulators and the sample
/// counter the envelopes are evaluated against.
///
/// The clock domains are nested. The sampler, filter and amplitude run per sample; the pitch, the
/// LFOs and the filter coefficients refresh on the 100 Hz control tick, which lands every ten
/// blocks; and pitch bend is re-read at the block boundary, which is the grid the engine applies
/// events on.
class PartialVoice {
public:
    /// Samples a choked voice holds at full level before it fades.
    static constexpr int choke_hold = 128;

    /// Samples a choked voice takes to fade out.
    static constexpr int choke_fade = 192;

    PartialVoice(const Interpolator& interpolator, const TvfChain& tvf, const PanLaw& pan_law)
        : reader_(interpolator), tvf_(&tvf), pan_law_(&pan_law), pitch_ramp_(tvf.ramp_exp())
    {
    }

    [[nodiscard]] int channel() const noexcept { return channel_; }

    [[nodiscard]] int note() const noexcept { return note_; }

    /// The drum mute group this voice belongs to, or zero.
    [[nodiscard]] int mute_group() const noexcept { return mute_group_; }

    /// Whether the voice has stopped producing sound.
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    /// Whether note-off has been received.
    [[nodiscard]] bool released() const noexcept { return note_off_ >= 0; }

    /// The kit level, folded in downstream of the voice gain.
    [[nodiscard]] double level_gain() const noexcept { return level_gain_; }

    /// Starts a partial.
    void start(VoiceSetup&& setup);

    /// Starts the release, with the part's CC64 value when it engages.
    ///
    /// A drum ignores note-off: its ring is a fixed length set at note-on. A key-off layer (an
    /// envelope hold that never expires on its own) *fires* here instead of releasing: wave and
    /// envelopes start fresh on the next control tick with no release pending, the note-off having
    /// been consumed arming the fire. A voice whose delayed start has not fired yet is killed
    /// without ever sounding.
    void note_off(int damper = 0);

    /// Cuts the voice short with the engine's fast fade.
    ///
    /// Used when a voice is stolen and when a drum choke group fires. The cut is near-instant —
    /// full at the strike, most of the way down by 5 ms and silent by 10 ms — but it is not a hard
    /// stop, which would click.
    void choke() noexcept
    {
        if (choke_at_ < 0) {
            choke_at_ = sample_;
        }
    }

    /// Silences the voice immediately and releases its wave.
    void kill() noexcept;

    /// Slews the voice's pan toward the part's, once a control tick.
    ///
    /// Call this before `render` for the block, like `volume_gains` and for the same reason: it
    /// reads the sample clock to find the tick.
    void slew_pan(int part_pan) noexcept;

    /// The stereo gains for this voice, at the pan position the slew has reached.
    [[nodiscard]] std::pair<double, double> pan_gains() const noexcept;

    /// Slews the three send levels toward the part's, once a control tick.
    ///
    /// Same ordering requirement as `slew_pan`, and the same shape of smoother -- `voice_pan_smooth`
    /// @`180083be0` is the sends' equivalent of `voice_expr_smooth`. It moves the gain word by
    /// 8 of 1024 a tick, so a full-scale send change takes 40 ticks, 400 ms.
    void slew_sends(int reverb, int chorus, int delay) noexcept;

    /// The send levels in force, on the controller's own 0-127 scale but continuous.
    [[nodiscard]] double reverb_send() const noexcept { return sends_[0]; }
    [[nodiscard]] double chorus_send() const noexcept { return sends_[1]; }
    [[nodiscard]] double delay_send() const noexcept { return sends_[2]; }

    /// Renders one block.
    ///
    /// A block never straddles a control tick: a voice starts on the render-block grid and the tick
    /// is ten blocks long, so the coefficient refresh always lands on a block boundary.
    ///
    /// `matrix` is the part's control-matrix modulation, already scaled into each destination's own
    /// units, and it is passed rather than stored because it is not purely a property of the part:
    /// polyphonic aftertouch contributes to it, and that belongs to this voice's key.
    void render(std::span<float> destination,
                double bend_milli_semitones,
                const ControlMatrix::Modulation& matrix);

    /// Sets the part's live cutoff offset, in the 15-bit cutoff units the filter sums.
    void set_cutoff_offset(double offset) noexcept { cutoff_offset_ = offset; }

    /// Fills one block of per-sample part-volume gains from the anti-zipper ramp.
    ///
    /// Call this *before* `render` for the same block. It reads the voice's own sample clock to
    /// find the control tick, which is the grid `voice_ramp_target_aux` retargets on — once every
    /// ten blocks, not once a block, because the retarget quantises the ramp accumulator and doing
    /// it ten times too often would bias the glide.
    void volume_gains(int word, unsigned mask, std::span<double> gains) noexcept;

private:
    void release(int damper = 0);
    void control(double bend_milli_semitones, const ControlMatrix::Modulation& matrix, bool first);
    [[nodiscard]] double ratio(double bend_milli_semitones) const;

    /// The ratio a given pitch modulation implies, without disturbing the voice's own.
    [[nodiscard]] double ratio_with(double modulation, double bend_milli_semitones) const;

    /// Maps an absolute sample index onto the envelopes' own time base.
    ///
    /// The hold is a whole number of control ticks, so the mapping keeps the envelope clock on the
    /// same 320-sample grid the voice runs on.
    [[nodiscard]] std::int64_t envelope_sample(std::int64_t sample) const noexcept
    {
        return hold_samples_ == 0 ? sample : std::max<std::int64_t>(0, sample - hold_samples_);
    }

    [[nodiscard]] static double choke_gain(std::int64_t since) noexcept;

    WaveReader reader_;
    const TvfChain* tvf_;
    const PanLaw* pan_law_;

    std::optional<SegmentEnvelope> amplitude_;
    std::optional<SegmentEnvelope> cutoff_;
    std::optional<PitchEnvelopeRunner> pitch_envelope_;
    std::optional<LfoRunner> lfo1_;
    std::optional<LfoRunner> lfo2_;
    StateVariableFilter filter_;

    int channel_ = 0;
    int note_ = 0;
    int mute_group_ = 0;
    bool finished_ = true;

    int cutoff_base_ = 0;

    /// The part's cutoff modify offset, refreshed every block rather than latched at note-on.
    ///
    /// CC#74, NRPN `01 20` and `40 1x 32` all move this, and files move it *during* notes -- one
    /// commercial file sweeps it 268 times in five minutes. Sampling it once per note leaves every
    /// sounding voice behind the sweep, which is a difference at the scale of notes rather than of
    /// cycles.
    double cutoff_offset_ = 0.0;
    int resonance_byte_ = 0x40;
    int filter_type_ = 0;
    FilterTap tap_ = FilterTap::bypass;

    bool is_drum_ = false;
    bool drum_receives_note_off_ = false;
    double drum_base_ratio_ = 1.0;
    double base_pitch_ = 0.0;
    double native_pitch_ = 0.0;

    /// The pan the voice is aiming at, before the slew.
    [[nodiscard]] int pan_target(int part_pan) const noexcept;

    int pan_ = PanLaw::centre;
    bool pan_follows_part_ = false;
    int random_pan_ = -1;

    /// The pan position actually in force, which chases `pan_target` rather than jumping to it.
    ///
    /// `voice_expr_smooth` @`180083db0` moves it by at most two of 127 a control tick, so a
    /// hard-panned CC#10 mid-note sweeps across roughly 300 ms rather than snapping. Measured:
    /// `scdec panramp` puts CC#10 64->127 at 32 steps of two, one every 320 samples, 310 ms end to
    /// end. Unlike the volume fader this is a slew on the *position*, with the left and right gains
    /// falling out of the pan law at whatever position it has reached -- there is no accumulator
    /// and no per-sample buffer, and the pair is a scalar for the whole tick.
    int pan_position_ = PanLaw::centre;
    /// Whether the position has been seeded; a voice starts *at* its pan, it does not glide in.
    bool pan_seeded_ = false;

    /// The send levels in force, chasing the part's rather than jumping to them.
    ///
    /// Held on the controller's 0-127 scale rather than as gains, because all three laws are
    /// linear in the controller and the engine's step is a fixed distance -- so one rate covers
    /// them whatever each effect's full-scale gain happens to be.
    ///
    /// Measured for the *reverb* send: `scdec sendramp` puts CC#91 0 -> 127 at 127 steps of
    /// 8/1024 of gain, one every 320 samples, 1260 ms end to end.
    ///
    /// **Only the reverb send.** A voice carries four (gain, bus) slots -- dry left, dry right, the
    /// reverb send on bus 0x3c, and a third whose bus the part's `+0x13` picks. The bus-assign code
    /// makes that third slot a chorus send (bus 0x3d) or a delay send (bus 0x30) when `+0x13`
    /// exceeds 0x1f, and a direct bus route otherwise -- but `+0x13` is the part's own **index**,
    /// the engine has exactly 32 parts, and 31 is 0x1f. The condition can never hold: those two
    /// branches are dead code in this build, and the third slot is always a direct route to bus
    /// 16 + index.
    ///
    /// So chorus and delay never reach a bus per voice, and there is no per-voice smoother on them
    /// to model. They pass through unchanged here. Where they *are* applied is the 33-bus send
    /// matrix in `fx_process_block`, downstream, off the part's `+0x3e2` and `+0x44a` -- unmeasured,
    /// though the existence of `fx_reg_write_slew` suggests it smooths something of its own.
    std::array<double, 3> sends_{};
    bool sends_seeded_ = false;
    double level_gain_ = 1.0;

    /// The part-volume anti-zipper ramp — `voice_ctrl_ramp_b`'s state, one per voice.
    ///
    /// Per voice rather than per part because that is where the engine keeps it: each voice has its
    /// own accumulator seeded at note-on, so voices started at different points of a slide are at
    /// different places along it.
    ControlRamp volume_ramp_;

    std::int64_t sample_ = 0;
    std::int64_t note_off_ = -1;
    std::int64_t choke_at_ = -1;
    std::int64_t control_tick_ = 0;
    std::int64_t hold_samples_ = 0;
    bool half_damper_ = false;
    double glide_ = 0.0;
    int glide_step_ = 0;

    double ratio_ = 1.0;

    /// The pitch ramp and the state that drives it.
    ///
    /// The engine glides the sampler's read rate from the pitch a control block is entered with to
    /// the pitch it leaves with, writing a fresh increment every eight samples. The entry pitch is
    /// the envelope's level *before* the block's step, which for the first block is the envelope's
    /// start level -- a value that is otherwise never rendered, since applying only post-tick
    /// values skips straight past it.
    PitchRamp pitch_ramp_;
    double entry_ratio_ = 1.0;
    double slot_ratio_ = 1.0;
    int slot_remaining_ = 0;
    double tremolo_ = 1.0;
    double frequency_ = 0.0;
    double damping_ = 0.0;
    double pitch_modulation_ = 0.0;
};

} // namespace ts
