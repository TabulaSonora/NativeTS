#pragma once

#include "realtime/wave_reader.hpp"
#include "tabulasonora/interpolator.hpp"
#include "tabulasonora/lfo_engine.hpp"
#include "tabulasonora/partial_parameters.hpp"
#include "tabulasonora/pitch_chain.hpp"
#include "tabulasonora/pitch_ramp.hpp"
#include "tabulasonora/segment_envelope.hpp"
#include "tabulasonora/state_variable_filter.hpp"
#include "tabulasonora/tvf_chain.hpp"
#include "tabulasonora/wave_descriptor.hpp"

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

    int pan = PanLaw::centre;
    bool pan_follows_part = false;
    /// A resolved random pan for this strike, or nothing to use `pan`.
    std::optional<int> random_pan;
    /// The kit level, folded in downstream of the voice gain.
    double level_gain = 1.0;
    /// The drum mute group, or zero.
    int mute_group = 0;

    /// Sample at which a drum takes its release; -1 for a melodic voice.
    std::int64_t auto_release_samples = -1;
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

    /// The stereo gains for this voice against a part's pan.
    [[nodiscard]] std::pair<double, double> pan_gains(int part_pan) const noexcept;

    /// Renders one block.
    ///
    /// A block never straddles a control tick: a voice starts on the render-block grid and the tick
    /// is ten blocks long, so the coefficient refresh always lands on a block boundary.
    void render(std::span<float> destination, double bend_milli_semitones, double mod_wheel_depth);

    /// Sets the part's live cutoff offset, in the 15-bit cutoff units the filter sums.
    void set_cutoff_offset(double offset) noexcept { cutoff_offset_ = offset; }

private:
    void release(int damper = 0);
    void control(double bend_milli_semitones, double mod_wheel_depth, bool first);
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
    double drum_base_ratio_ = 1.0;
    double base_pitch_ = 0.0;
    double native_pitch_ = 0.0;

    int pan_ = PanLaw::centre;
    bool pan_follows_part_ = false;
    int random_pan_ = -1;
    double level_gain_ = 1.0;

    std::int64_t sample_ = 0;
    std::int64_t note_off_ = -1;
    std::int64_t choke_at_ = -1;
    std::int64_t auto_release_ = -1;
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
