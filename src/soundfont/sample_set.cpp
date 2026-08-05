#include "tabulasonora/soundfont_samples.hpp"
#include "dsp/fixed.hpp"
#include "dsp/wave_codec.hpp"

#include <algorithm>
#include <cstdlib>

#include <cmath>
#include <set>
#include <utility>

namespace ts::sf2 {
namespace {

/// Absolute ROM extent of a wave, which is what two waves must share to be interchangeable.
struct Extent {
    int region = 0;
    int begin = 0;
    int end = 0;

    [[nodiscard]] friend bool operator<(const Extent& a, const Extent& b) noexcept
    {
        return std::tie(a.region, a.begin, a.end) < std::tie(b.region, b.begin, b.end);
    }
};

[[nodiscard]] Extent extent_of(const WaveDescriptor& descriptor) noexcept
{
    return Extent{descriptor.region, descriptor.loop, descriptor.start};
}

/// Collects every wave a tone's key zones can select.
void add_tone_waves(const PatchDirectory& directory, int tone_number, std::set<int>& into)
{
    std::vector<ToneZone> zones;
    if (!directory.tone_zones(tone_number, zones)) {
        return;
    }
    for (const ToneZone& zone : zones) {
        into.insert(zone.wave);
    }
}

/// The samples the engine's sampler walks for a wave, before any interpolation.
///
/// This is the ground truth a bake has to reproduce: the same values in the same order, with the
/// loop or the ping-pong traversal applied by index rather than by resampling. It mirrors
/// `WaveReader`'s read paths -- including `generate`'s leg machine, whose turnaround leaves the
/// index unchanged so that sample's delta is integrated twice.
[[nodiscard]] std::vector<float> engine_samples(const DecodedWave& wave, int count)
{
    std::vector<float> output;
    output.reserve(static_cast<std::size_t>(std::max(0, count)));

    if (wave.mode == SamplerMode::ping_pong) {
        std::int32_t predictor = wave.seed;
        int index = 0;
        int leg = 0;
        bool ended = false;

        for (int i = 0; i < count; ++i) {
            if (ended) {
                output.push_back(0.0F);
                continue;
            }
            const int current = index;
            switch (leg) {
            case 0:
                ++index;
                if (index > wave.data_end) {
                    if (wave.data_end <= wave.loop_start) {
                        ended = true;
                    } else {
                        leg = 1;
                        index = wave.data_end;
                    }
                }
                break;
            case 1:
                --index;
                if (index < wave.loop_start) {
                    leg = 2;
                    index = wave.loop_start;
                }
                break;
            default:
                ++index;
                if (index > wave.data_end) {
                    leg = 1;
                    index = wave.data_end;
                }
                break;
            }
            predictor = fx::wadd(predictor, wave.steps[static_cast<std::size_t>(current)]);
            output.push_back(static_cast<float>(static_cast<double>(predictor)
                                                * codec::output_scale));
        }
        return output;
    }

    const auto size = static_cast<int>(wave.samples.size());
    for (int i = 0; i < count; ++i) {
        int index = i;
        if (wave.is_looping() && index > wave.data_end) {
            const int period = wave.loop_period();
            index = wave.loop_start + ((index - (wave.data_end + 1)) % period);
        }
        output.push_back(index < size ? wave.samples[static_cast<std::size_t>(index)] : 0.0F);
    }
    return output;
}

} // namespace

RootTuning root_tuning(const WaveDescriptor& descriptor) noexcept
{
    // The descriptor tunes in milli-semitones; a semitone is 100 cents, so a milli-semitone is
    // a tenth of a cent.
    const double semitones = descriptor.native_milli_semitones() / 1000.0;
    const auto key = static_cast<int>(std::lround(semitones));

    // **Negated**, and the sign is the whole point. SF2's `chPitchCorrection` compensates for a
    // recording error rather than describing the sample's pitch: it is added to playback, so a
    // sample sitting *sharp* of its nominal root needs a *negative* correction to be pulled back.
    //
    // Wave 7 is the worked example. Its native pitch is 64052 milli-semitones -- 5.2 cents above
    // root key 64 -- so the correction that brings it to pitch is -5, not +5. Emitting +5 makes it
    // 10.4 cents sharp instead of correct, and getting this backwards detunes the entire bank by
    // twice each sample's own offset. Measured before the fix: +11.5 cents on a rendered middle C,
    // and +9.9 to +12.2 cents in the pitch law across the keyboard.
    const auto cents = static_cast<int>(std::lround((key - semitones) * 100.0));
    return RootTuning{key, cents};
}

Bake bake_for(const WaveDescriptor& descriptor, const DecodedWave& wave) noexcept
{
    if (descriptor.ping_pong() && wave.mode == SamplerMode::ping_pong) {
        return Bake::ping_pong;
    }
    if (descriptor.reverse()) {
        return Bake::reverse;
    }
    return wave.is_looping() ? Bake::forward_loop : Bake::one_shot;
}

std::vector<int> SampleSet::census(const PatchDirectory& directory, const DrumKitTable& kits)
{
    std::set<int> tones;

    for (const auto& [name, selector] : tone_map_choices()) {
        const auto map = static_cast<ToneMap>(selector);
        for (int bank = 0; bank < 128; ++bank) {
            for (int program = 0; program < 128; ++program) {
                for (const int tone_number : directory.program_tones(program, map, bank)) {
                    tones.insert(tone_number);
                }
            }
        }
    }

    // Drum kits reference ordinary melodic tones, one per sounding key. A key that receives no
    // note-on can never sound, so its tone is not part of the export.
    for (int kit = 0; kit < kits.kit_count(); ++kit) {
        for (int note = 0; note < DrumKitTable::key_count; ++note) {
            const DrumKey key = kits.key(note, kit);
            if (key.receives_note_on && key.tone > 0) {
                tones.insert(key.tone);
            }
        }
    }

    std::set<int> waves;
    for (const int tone_number : tones) {
        add_tone_waves(directory, tone_number, waves);
    }

    return {waves.begin(), waves.end()};
}

SampleSet SampleSet::build(const PatchDirectory& directory,
                           Sampler& sampler,
                           std::span<const int> waves,
                           bool share_identical)
{
    SampleSet set;

    // Waves that decode to the same bytes from the same origin are interchangeable. Keying on the
    // ROM extent alone is not enough -- two waves over overlapping ROM have identical shape but
    // can differ by a DC constant -- so the extent must match exactly, which is what makes the
    // predictor's origin match too.
    std::map<std::pair<Extent, Bake>, int> by_extent;

    for (const int wave_number : waves) {
        const std::optional<WaveDescriptor> descriptor = directory.wave(wave_number);
        if (!descriptor) {
            set.skipped_.push_back(wave_number);
            continue;
        }

        const DecodedWave* decoded = sampler.decode(*descriptor);
        if (decoded == nullptr || decoded->samples.empty()) {
            set.skipped_.push_back(wave_number);
            continue;
        }

        const Bake bake = bake_for(*descriptor, *decoded);
        const auto key = std::pair{extent_of(*descriptor), bake};

        if (share_identical) {
            if (const auto found = by_extent.find(key); found != by_extent.end()) {
                set.wave_to_run_.emplace(wave_number, found->second);
                ++set.shared_count_;
                continue;
            }
        }

        const auto [root, cents] = root_tuning(*descriptor);
        SampleRun run{
            .wave = wave_number,
            .bake = bake,
            .region = descriptor->region,
            .rom_start = descriptor->loop,
            .rom_end = descriptor->start,
            .pool_offset = static_cast<std::int64_t>(set.pool_.size()),
            .length = 0,
            .loop_start = 0,
            .loop_end = 0,
            .root_key = root,
            .fine_cents = cents,
        };

        switch (bake) {
        case Bake::ping_pong: {
            // The traversal is ascending, then descending, then ascending again, so a round trip
            // starting at the *ascending* leg is the wave's own samples over [loop_start,
            // data_end] followed by the descending leg. That means only the descending leg is new
            // data: the ascending half is already there as the tail of the decoded wave, and the
            // loop can start at loop_start where it always did.
            //
            // This works because the ascending half of a round trip really is the forward decode.
            // The round trip's net predictor change is zero -- measured, no drift over three
            // passes on any of the 612 -- and the descending leg contributes the same delta sum as
            // the ascending one, so each is individually zero and the predictor entering an
            // ascending leg equals the forward predictor at loop_start.
            //
            // The naive layout instead appends a whole round trip after the whole wave, which
            // stores the ascending half twice: 40.72 MB against 28.96 MB over the set.
            //
            // The traversal must be taken in the sample domain. `Sampler::play` resamples even at
            // unity rate -- its 4-tap kernel is not an identity at integer positions -- so baking
            // from it stores a filtered wave and the loop no longer joins.
            const int down = decoded->data_end - decoded->loop_start;
            const int lead_in = decoded->data_end + 1;
            const std::vector<float> traversal = engine_samples(*decoded, lead_in + down + 1);
            if (static_cast<int>(traversal.size()) < lead_in + down + 1) {
                set.skipped_.push_back(wave_number);
                continue;
            }

            set.pool_.insert(set.pool_.end(), decoded->samples.begin(), decoded->samples.end());
            set.pool_.insert(set.pool_.end(), traversal.begin() + lead_in,
                             traversal.begin() + lead_in + down + 1);

            run.length = static_cast<int>(decoded->samples.size()) + down + 1;
            run.loop_start = decoded->loop_start;
            run.loop_end = run.length;
            break;
        }

        case Bake::forward_loop:
            // The decoder's samples are already the forward run; the loop is inclusive of the data
            // end, matching DecodedWave::loop_period.
            set.pool_.insert(set.pool_.end(), decoded->samples.begin(), decoded->samples.end());
            run.length = static_cast<int>(decoded->samples.size());
            run.loop_start = decoded->loop_start;
            run.loop_end = decoded->data_end + 1;
            break;

        case Bake::reverse:
        case Bake::one_shot:
            // A reverse wave is already turned round by the decoder, so both are plain one-shots.
            set.pool_.insert(set.pool_.end(), decoded->samples.begin(), decoded->samples.end());
            run.length = static_cast<int>(decoded->samples.size());
            break;
        }

        const auto index = static_cast<int>(set.runs_.size());
        set.runs_.push_back(run);
        by_extent.emplace(key, index);
        set.wave_to_run_.emplace(wave_number, index);
    }

    return set;
}

std::vector<float> read_run(const SampleSet& set, const SampleRun& run, int sample_count)
{
    std::vector<float> output;
    output.reserve(static_cast<std::size_t>(std::max(0, sample_count)));

    const std::span<const float> pool = set.pool();
    const int period = run.loop_end - run.loop_start;

    for (int i = 0; i < sample_count; ++i) {
        int index = i;
        if (run.loops() && index >= run.loop_end) {
            index = run.loop_start + ((index - run.loop_start) % period);
        } else if (!run.loops() && index >= run.length) {
            output.push_back(0.0F);
            continue;
        }
        output.push_back(pool[static_cast<std::size_t>(run.pool_offset + index)]);
    }

    return output;
}

double verify_run(const SampleSet& set,
                  const SampleRun& run,
                  const PatchDirectory& directory,
                  Sampler& sampler,
                  int sample_count)
{
    const std::optional<WaveDescriptor> descriptor = directory.wave(run.wave);
    if (!descriptor) {
        return -1.0;
    }
    const DecodedWave* decoded = sampler.decode(*descriptor);
    if (decoded == nullptr) {
        return -1.0;
    }

    // The reference has to be built in the *sample* domain. `Sampler::play` is not an identity at
    // unity rate -- its 4-tap interpolator is a windowed kernel, not a Lagrange that passes
    // integer positions, so it returns a filtered wave (measured: sample 0 of wave 271 reads
    // 0.015625 raw and -0.237263 played). Comparing a bake against it measures the interpolator.
    const std::vector<float> reference = engine_samples(*decoded, sample_count);
    const std::vector<float> baked = read_run(set, run, sample_count);

    double worst = 0.0;
    const auto count = std::min(reference.size(), baked.size());
    for (std::size_t i = 0; i < count; ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(reference[i] - baked[i])));
    }
    return worst;
}

int SampleSet::run_for_wave(int wave) const noexcept
{
    const auto found = wave_to_run_.find(wave);
    return found == wave_to_run_.end() ? -1 : found->second;
}

} // namespace ts::sf2
