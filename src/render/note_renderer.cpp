#include "tabulasonora/note_renderer.hpp"

#include "tabulasonora/effect_presets.hpp"

#include "tabulasonora/sequence.hpp"

#include "dsp/simd.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ts {

/// Everything the renderer owns.
///
/// Declaration order is load-bearing: each chain holds references to the tables and the shared
/// machine, so those have to be constructed first. The upstream C# gets this for free from the GC;
/// here it is the member order.
struct NoteRenderer::Impl {
    explicit Impl(const RomImage& rom)
        : rom(&rom),
          tables(TableSet::from_rom(rom)),
          envelopes(tables),
          directory(tables),
          drums(rom),
          wave_rom(rom),
          interpolator(tables),
          sampler(wave_rom, interpolator),
          tva(tables, envelopes),
          tvf(tables, envelopes),
          pitch(tables, envelopes, &noise),
          lfo(tables),
          pan(tables)
    {
        // The effect coefficients come out of the same file as everything else, so opening the
        // renderer is what makes the send effects available -- there is no preset file to find.
        EffectPresets::ensure_from(rom);
    }

    TableSet tables;
    EnvelopeMachine envelopes;
    EngineNoise noise;
    PatchDirectory directory;
    DrumKitTable drums;
    const RomImage* rom;
    WaveRom wave_rom;
    Interpolator interpolator;
    Sampler sampler;
    TvaChain tva;
    TvfChain tvf;
    PitchChain pitch;
    LfoEngine lfo;
    PanLaw pan;

    /// Renders one partial, or nothing when it does not sound.
    [[nodiscard]] std::optional<std::vector<float>>
    render_partial(int tone_number,
                   const PartialParameters& partial,
                   const WaveDescriptor& descriptor,
                   int note,
                   int velocity,
                   double hold_seconds,
                   double tail_seconds,
                   std::size_t sample_count,
                   std::span<const double> pitch_add,
                   std::span<const double> mod_wheel_per_tick,
                   std::optional<int> drum_coarse_pitch = std::nullopt,
                   std::optional<int> envelope_rate_key = std::nullopt);
};

namespace {

/// Applies the part volume curve to all three buffers.
///
/// The send is post-fader: the wet scales with volume too. Split at the curve's end rather than
/// clamping the index per sample -- the curve is built per note and can be shorter than the buffer,
/// in which case the tail is one constant gain.
void apply_volume(std::span<float> left,
                  std::span<float> right,
                  std::span<float> mono,
                  std::span<const double> volume)
{
    if (volume.empty()) {
        return;
    }

    const auto apply = [volume](std::span<float> channel) {
        const std::size_t moving = std::min(channel.size(), volume.size());
        simd::scale_varying(channel.first(moving), volume);
        if (moving < channel.size()) {
            simd::scale(channel.subspan(moving), volume.back());
        }
    };

    apply(left);
    apply(right);
    apply(mono);
}

/// Trims the trailing colons and spaces the tone names carry.
[[nodiscard]] std::string trim_name(std::string name)
{
    const auto last = name.find_last_not_of(": ");
    name.erase(last == std::string::npos ? 0 : last + 1);
    return name;
}

} // namespace

NoteRenderer::NoteRenderer(const RomImage& rom) : impl_(std::make_unique<Impl>(rom)) {}

NoteRenderer::NoteRenderer(NoteRenderer&&) noexcept = default;
NoteRenderer& NoteRenderer::operator=(NoteRenderer&&) noexcept = default;
NoteRenderer::~NoteRenderer() = default;

const RomImage& NoteRenderer::rom() const noexcept
{
    return *impl_->rom;
}

const TableSet& NoteRenderer::tables() const noexcept
{
    return impl_->tables;
}

EngineNoise& NoteRenderer::noise() noexcept
{
    return impl_->noise;
}

const PatchDirectory& NoteRenderer::directory() const noexcept
{
    return impl_->directory;
}

const DrumKitTable& NoteRenderer::drums() const noexcept
{
    return impl_->drums;
}

const Interpolator& NoteRenderer::interpolator() const noexcept
{
    return impl_->interpolator;
}

Sampler& NoteRenderer::sampler() noexcept
{
    return impl_->sampler;
}

const EnvelopeMachine& NoteRenderer::envelopes() const noexcept
{
    return impl_->envelopes;
}

const TvaChain& NoteRenderer::tva() const noexcept
{
    return impl_->tva;
}

const TvfChain& NoteRenderer::tvf() const noexcept
{
    return impl_->tvf;
}

const PitchChain& NoteRenderer::pitch() const noexcept
{
    return impl_->pitch;
}

const LfoEngine& NoteRenderer::lfo() const noexcept
{
    return impl_->lfo;
}

const PanLaw& NoteRenderer::pan() const noexcept
{
    return impl_->pan;
}

double NoteRenderer::drum_ring_scale(const DrumKey& key) noexcept
{
    const double ratio = DrumKitTable::coarse_pitch_ratio(key.pitch);
    return ratio <= 0 ? 1.0 : std::clamp(1.0 / ratio, 1.0, max_drum_ring_scale);
}

double NoteRenderer::drum_ring_scale(int note, int kit, int drum_pitch) const
{
    return drum_ring_scale(
        DrumKeyOverrides::apply(impl_->drums.key(note, kit), drum_pitch, std::nullopt));
}

RenderedNote NoteRenderer::render_drum_note(int note,
                                            int velocity,
                                            double ring_seconds,
                                            double tail_seconds,
                                            int kit,
                                            std::span<const double> volume,
                                            int drum_pitch,
                                            std::optional<int> drum_pan)
{
    // The kit's own key is kept alongside the overridden one: the envelope rate key-follow is
    // indexed from the stored plane plus the NRPN offset in semitones, not from the plane after the
    // override has doubled that offset into it.
    const DrumKey kit_key = impl_->drums.key(note, kit);
    const DrumKey key = DrumKeyOverrides::apply(kit_key, drum_pitch, drum_pan);

    // The ring stretches with the coarse pitch, and the hold has to stretch with it: the hold is
    // where note-off lands, and both the TVA and TVF envelopes are sized from it. Leaving it at the
    // nominal ring builds envelopes shorter than the signal they gate.
    const double ring = ring_seconds * drum_ring_scale(key);
    const auto sample_count =
        static_cast<std::size_t>(std::max(0.0, (ring + tail_seconds) * sample_rate));

    // A drum sounds its tone at key 60; the plane, not the note, supplies the pitch.
    const ResolvedTone resolved = impl_->directory.resolve(key.tone, /*note=*/60, velocity);

    RenderedNote out;
    out.left.assign(sample_count, 0.0F);
    out.right.assign(sample_count, 0.0F);
    out.mono.assign(sample_count, 0.0F);
    out.name = resolved.name;

    const std::optional<Tone> tone = impl_->directory.tone(key.tone);
    if (sample_count == 0 || resolved.partials.empty() || !tone) {
        return out;
    }

    for (const ResolvedPartial& sounding : resolved.partials) {
        const PartialParameters& partial =
            tone->partials()[static_cast<std::size_t>(sounding.partial_index)];

        const std::optional<std::vector<float>> signal =
            impl_->render_partial(key.tone,
                                  partial,
                                  sounding.descriptor,
                                  /*note=*/60,
                                  velocity,
                                  ring,
                                  tail_seconds,
                                  sample_count,
                                  {},
                                  {},
                                  key.pitch,
                                  envelope_rate_key(kit_key, drum_pitch));

        if (signal) {
            simd::add(*signal, out.mono);
        }
    }

    // The kit level acts as (level/127) squared, and pan is per drum key.
    const double level_gain = DrumKitTable::level_gain(key.level);
    const auto [gain_left, gain_right] = impl_->pan.gains(key.pan);

    // Pan reads the levelled mono back after it has been narrowed to float, as the scalar loop did
    // -- the kit level is not a second factor folded into the pan gain.
    simd::scale(out.mono, level_gain);
    simd::store_scaled(out.mono, gain_left, out.left);
    simd::store_scaled(out.mono, gain_right, out.right);

    apply_volume(out.left, out.right, out.mono, volume);
    return out;
}

int NoteRenderer::envelope_rate_key(const DrumKey& key, int drum_pitch) noexcept
{
    return std::clamp(key.pitch + drum_pitch, 0, 0x7F);
}

std::vector<double> NoteRenderer::expand(std::span<const double> ticks, std::size_t sample_count)
{
    // Walked a control block at a time rather than a sample at a time: per sample it would cost an
    // integer division to recover a tick index that only advances every control_block samples.
    std::vector<double> result(sample_count);
    if (ticks.empty()) {
        return result;
    }

    for (std::size_t start = 0; start < sample_count; start += control_block) {
        const std::size_t length = std::min<std::size_t>(control_block, sample_count - start);
        const std::size_t tick = std::min(start / control_block, ticks.size() - 1);
        std::fill_n(result.begin() + static_cast<std::ptrdiff_t>(start), length, ticks[tick]);
    }

    return result;
}

RenderedNote NoteRenderer::render_note(int program,
                                       int note,
                                       int velocity,
                                       double hold_seconds,
                                       double tail_seconds,
                                       ToneMap map,
                                       int bank,
                                       int part_pan,
                                       const Controllers& controllers)
{
    const auto sample_count =
        static_cast<std::size_t>(std::max(0.0, (hold_seconds + tail_seconds) * sample_rate));

    const std::vector<int> tones = impl_->directory.program_tones(program, map, bank);
    if (tones.empty() || sample_count == 0) {
        return RenderedNote{std::vector<float>(sample_count),
                            std::vector<float>(sample_count),
                            std::vector<float>(sample_count),
                            "(unassigned)"};
    }

    RenderedNote out;
    out.left.assign(sample_count, 0.0F);
    out.right.assign(sample_count, 0.0F);
    out.mono.assign(sample_count, 0.0F);
    out.name = "(none)";

    for (int tone_number : tones) {
        const ResolvedTone resolved = impl_->directory.resolve(tone_number, note, velocity);
        out.name = trim_name(resolved.name);

        const std::optional<Tone> tone = impl_->directory.tone(tone_number);
        if (!tone) {
            continue;
        }

        for (const ResolvedPartial& sounding : resolved.partials) {
            const PartialParameters& partial =
                tone->partials()[static_cast<std::size_t>(sounding.partial_index)];

            const std::optional<std::vector<float>> signal =
                impl_->render_partial(tone_number,
                                      partial,
                                      sounding.descriptor,
                                      note,
                                      velocity,
                                      hold_seconds,
                                      tail_seconds,
                                      sample_count,
                                      controllers.pitch_add,
                                      controllers.mod_wheel_per_tick);

            if (!signal) {
                continue;
            }

            // Partials sum. There is no divide-by-count anywhere in this path.
            const auto [gain_left, gain_right] =
                impl_->pan.gains(partial.pan() + (part_pan - 0x40));
            simd::mix_scaled(*signal, gain_left, out.left);
            simd::mix_scaled(*signal, gain_right, out.right);
            simd::add(*signal, out.mono);
        }
    }

    apply_volume(out.left, out.right, out.mono, controllers.volume);
    return out;
}

std::optional<std::vector<float>>
NoteRenderer::Impl::render_partial(int tone_number,
                                   const PartialParameters& partial,
                                   const WaveDescriptor& descriptor,
                                   int note,
                                   int velocity,
                                   double hold_seconds,
                                   double tail_seconds,
                                   std::size_t sample_count,
                                   std::span<const double> pitch_add,
                                   std::span<const double> mod_wheel_per_tick,
                                   std::optional<int> drum_coarse_pitch,
                                   std::optional<int> envelope_rate_key)
{
    const DecodedWave* wave = sampler.decode(descriptor);
    if (wave == nullptr) {
        return std::nullopt;
    }

    const int key = std::clamp(note, 0, 0x7F);

    // The envelope hold clock. An armed voice renders nothing at all -- the wave's read position
    // and every control value stay frozen -- and when the clock fires the whole voice simply
    // starts. A delayed layer is therefore the same voice time-shifted by the delay.
    const std::int64_t hold = envelopes.hold_samples(partial, velocity);
    std::size_t start_at = 0;
    double local_hold = hold_seconds;
    double local_tail = tail_seconds;

    if (hold == EnvelopeMachine::hold_forever) {
        const std::int64_t fire_at =
            (static_cast<std::int64_t>(hold_seconds * sample_rate) + control_block - 1)
            / control_block * control_block;
        start_at = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(sample_count), fire_at));
        // Never released in-window.
        local_hold = (static_cast<double>(sample_count - start_at) / sample_rate) + 1.0;
        local_tail = 0.0;
    } else if (hold > 0) {
        if (hold_seconds * sample_rate <= static_cast<double>(hold)) {
            // Note-off landed while the delay was still running: the engine kills the armed voice
            // before it has ever sounded.
            return std::nullopt;
        }
        start_at = static_cast<std::size_t>(hold);
        local_hold = hold_seconds - (static_cast<double>(start_at) / sample_rate);
    }

    if (start_at >= sample_count) {
        return std::nullopt;
    }
    const std::size_t span = sample_count - start_at;

    const auto tick_count =
        static_cast<int>(std::ceil(static_cast<double>(span) / control_block) + 1);

    const std::optional<std::vector<double>> envelope =
        pitch.envelope_ticks(partial, key, velocity, local_hold, tick_count);

    // The mod wheel adds vibrato depth to LFO1 only, and only while it is actually moved.
    const std::vector<double> lfo_pitch =
        mod_wheel_per_tick.empty()
            ? lfo.modulation(tone_number, partial, tick_count, LfoDestination::pitch)
            : lfo.pitch_modulation_with_wheel(tone_number, partial, tick_count, mod_wheel_per_tick);

    const std::vector<double> pitch_envelope =
        envelope ? NoteRenderer::expand(*envelope, span) : std::vector<double>{};
    const std::vector<double> pitch_lfo = simd::any_non_zero(lfo_pitch)
                                              ? NoteRenderer::expand(lfo_pitch, span)
                                              : std::vector<double>{};

    std::vector<double> ratios(span);

    if (drum_coarse_pitch) {
        // Drums take a different pitch route: the note does not transpose the sample. The kit's
        // coarse-pitch plane supplies the key instead, and the tone's own key-follow decides what a
        // step of it is worth. There is no absolute-pitch accumulator to clamp on this path.
        const double native = (descriptor.root_key * 1000.0) + 1024.0 - descriptor.fine_tune;
        const double base_ratio =
            std::pow(2.0,
                     (PitchChain::drum_pitch_milli_semitones(partial, *drum_coarse_pitch) - native)
                         / 12000.0);

        double last_modulation = std::numeric_limits<double>::quiet_NaN();
        double last_ratio = 0.0;
        for (std::size_t i = 0; i < span; ++i) {
            double modulation = 0.0;
            if (!pitch_envelope.empty()) {
                modulation += pitch_envelope[i];
            }
            if (!pitch_lfo.empty()) {
                modulation += pitch_lfo[i];
            }

            if (modulation != last_modulation) {
                last_ratio = base_ratio * std::pow(2.0, modulation / 12000.0);
                last_modulation = modulation;
            }
            ratios[i] = last_ratio;
        }
    } else {
        // Melodic: an absolute pitch in milli-semitones, clamped as the engine's accumulator is,
        // then taken as a ratio against the sample's own root.
        const auto base_pitch = static_cast<double>(
            pitch.base_pitch_milli_semitones(partial, note, partial.key_center()));
        const double native = (descriptor.root_key * 1000.0) + 1024.0 - descriptor.fine_tune;

        // Memoised on the exact input. Both modulation terms come from expand(), so they hold one
        // value across each control block; only bend moves per sample, and it is absent on a note
        // that never bends. Reusing only on a bit-identical input keeps this exact.
        double last_pitch = std::numeric_limits<double>::quiet_NaN();
        double last_ratio = 0.0;
        for (std::size_t i = 0; i < span; ++i) {
            double value = base_pitch;
            if (!pitch_envelope.empty()) {
                value += pitch_envelope[i];
            }
            if (!pitch_lfo.empty()) {
                value += pitch_lfo[i];
            }
            if (!pitch_add.empty()) {
                // The curve is in absolute note time; a held voice reads it from where it fires.
                value += pitch_add[std::min(start_at + i, pitch_add.size() - 1)];
            }

            if (value != last_pitch) {
                last_ratio = std::pow(2.0, (PitchChain::clamp(value) - native) / 12000.0);
                last_pitch = value;
            }
            ratios[i] = last_ratio;
        }
    }

    std::vector<float> signal = sampler.play_variable(*wave, static_cast<int>(span), ratios);

    // --- filter: the cutoff envelope with the LFO added, then clamped, as the engine does ---
    std::vector<double> cutoff = tvf.envelope(partial, velocity, key, local_hold, local_tail);
    const std::vector<double> lfo_tvf =
        lfo.modulation(tone_number, partial, tick_count, LfoDestination::tvf);
    if (simd::any_non_zero(lfo_tvf)) {
        const std::vector<double> expanded = NoteRenderer::expand(lfo_tvf, cutoff.size());
        for (std::size_t i = 0; i < cutoff.size(); ++i) {
            cutoff[i] = std::clamp(cutoff[i] + expanded[i], 0.0, static_cast<double>(0x7FFF));
        }
    }

    tvf.apply(signal, cutoff, partial.filter_type(), TvfChain::resonance_byte(partial));

    // --- amplitude ---
    const int zone_level = directory.zone_level(partial.multisample(), key, partial.key_center());
    const std::vector<float> amplitude = tva.render(partial,
                                                    velocity,
                                                    key,
                                                    local_hold,
                                                    local_tail,
                                                    zone_level,
                                                    directory.tone_level(tone_number),
                                                    NoteRenderer::sample_rate,
                                                    0.0,
                                                    envelope_rate_key);

    const std::vector<double> lfo_tva =
        lfo.modulation(tone_number, partial, tick_count, LfoDestination::tva);
    const std::vector<double> tremolo =
        simd::any_non_zero(lfo_tva) ? NoteRenderer::expand(lfo_tva, span) : std::vector<double>{};

    for (std::size_t i = 0; i < span && i < amplitude.size(); ++i) {
        auto gain = static_cast<double>(amplitude[i]);
        if (!tremolo.empty()) {
            // Amplitude modulation folds in as a fraction of 0x7f00, clamped first.
            gain *=
                1.0
                + (std::clamp(tremolo[i], -static_cast<double>(0x7F00), static_cast<double>(0x7F00))
                   / static_cast<double>(0x7F00));
        }
        signal[i] = static_cast<float>(static_cast<double>(signal[i]) * gain);
    }

    if (start_at == 0) {
        return signal;
    }

    // A held voice is the same voice starting late: silence to the fire point, the rendered span
    // after it.
    std::vector<float> shifted(sample_count, 0.0F);
    std::copy(signal.begin(),
              signal.begin() + static_cast<std::ptrdiff_t>(span),
              shifted.begin() + static_cast<std::ptrdiff_t>(start_at));
    return shifted;
}

} // namespace ts
