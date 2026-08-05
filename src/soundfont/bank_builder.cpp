#include "tabulasonora/soundfont_bank.hpp"

#include "tabulasonora/lfo_engine.hpp"
#include "tabulasonora/pitch_chain.hpp"
#include "tabulasonora/soundfont_envelopes.hpp"
#include "tabulasonora/tvf_chain.hpp"
#include "tabulasonora/tone.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace ts::sf2 {
namespace {

/// Zero samples between one sample's data and the next, as the specification asks.
///
/// A reader that honours `dwStart`/`dwEnd` never reads into it, but a 4-tap interpolator running
/// off the end of a one-shot does, and the gap is what it finds instead of the next sample.
constexpr int inter_sample_gap = 46;

/// Trims a ROM name to something that reads well in a preset list.
[[nodiscard]] std::string tidy(std::string name)
{
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) {
        name.pop_back();
    }
    return name;
}

/// Attenuation in centibels for a linear gain, clamped to the range SF2 can express.
[[nodiscard]] int centibels(double gain) noexcept
{
    if (gain <= 0.0) {
        return 1440;
    }
    const double cb = -200.0 * std::log10(gain);
    return static_cast<int>(std::lround(std::clamp(cb, 0.0, 1440.0)));
}

/// The LFO's rate in Hz.
///
/// The configured increment steps a 16-bit phase once per control tick, and the control tick is
/// 100 Hz, so a full cycle is `0x10000 / increment` ticks.
[[nodiscard]] double lfo_hz(int increment) noexcept
{
    return (static_cast<double>(increment) * 100.0) / 65536.0;
}

/// The LFO's delay in seconds, or a negative value when it never starts.
///
/// The delay accumulator steps by `delay_rate` per tick and the LFO begins when it wraps. A rate of
/// zero is not "no delay" -- it is an LFO that never arrives, which the engine treats as off.
[[nodiscard]] double lfo_delay_seconds(int delay_rate) noexcept
{
    if (delay_rate <= 0) {
        return -1.0;
    }
    return 65536.0 / (static_cast<double>(delay_rate) * 100.0);
}

/// Absolute cents for a frequency, which is what `initialFilterFc` holds.
///
/// 8.176 Hz is SF2's reference, the frequency of MIDI note 0.
[[nodiscard]] int absolute_cents(double hz) noexcept
{
    if (!(hz > 0.0)) {
        return 1500;
    }
    return static_cast<int>(
        std::lround(std::clamp(1200.0 * std::log2(hz / 8.176), 1500.0, 13500.0)));
}

/// Writes a fitted modulation envelope as its generators, omitting SF2 defaults.
void emit_mod_dahdsr(std::vector<Generator>& into, const Dahdsr& envelope)
{
    if (envelope.delay > min_timecents) {
        into.push_back(Generator::value(Gen::delay_mod_env, envelope.delay));
    }
    if (envelope.attack > min_timecents) {
        into.push_back(Generator::value(Gen::attack_mod_env, envelope.attack));
    }
    if (envelope.hold > min_timecents) {
        into.push_back(Generator::value(Gen::hold_mod_env, envelope.hold));
    }
    if (envelope.decay > min_timecents) {
        into.push_back(Generator::value(Gen::decay_mod_env, envelope.decay));
    }
    if (envelope.sustain != 0) {
        into.push_back(Generator::value(Gen::sustain_mod_env, envelope.sustain));
    }
    if (envelope.release > min_timecents) {
        into.push_back(Generator::value(Gen::release_mod_env, envelope.release));
    }
}

/// Writes a fitted DAHDSR as its six generators, omitting any left at the SF2 default.
///
/// Omitting defaults is not only tidiness: a zone generator overrides the instrument's, so writing
/// a default explicitly would stop a future global-zone value from reaching this zone.
void emit_dahdsr(std::vector<Generator>& into, const Dahdsr& envelope)
{
    if (envelope.delay > min_timecents) {
        into.push_back(Generator::value(Gen::delay_vol_env, envelope.delay));
    }
    if (envelope.attack > min_timecents) {
        into.push_back(Generator::value(Gen::attack_vol_env, envelope.attack));
    }
    if (envelope.hold > min_timecents) {
        into.push_back(Generator::value(Gen::hold_vol_env, envelope.hold));
    }
    if (envelope.decay > min_timecents) {
        into.push_back(Generator::value(Gen::decay_vol_env, envelope.decay));
    }
    if (envelope.sustain != 0) {
        into.push_back(Generator::value(Gen::sustain_vol_env, envelope.sustain));
    }
    if (envelope.release > min_timecents) {
        into.push_back(Generator::value(Gen::release_vol_env, envelope.release));
    }
}

/// The tones the export reaches: every mapped melodic tone, plus every tone a drum key sounds.
[[nodiscard]] std::set<int> reachable_tones(const PatchDirectory& directory,
                                            const DrumKitTable& kits)
{
    std::set<int> tones;
    for (const auto& [name, selector] : tone_map_choices()) {
        const auto map = static_cast<ToneMap>(selector);
        for (int bank = 0; bank < 128; ++bank) {
            for (int program = 0; program < 128; ++program) {
                for (const int tone : directory.program_tones(program, map, bank)) {
                    tones.insert(tone);
                }
            }
        }
    }
    for (int kit = 0; kit < kits.kit_count(); ++kit) {
        for (int note = 0; note < DrumKitTable::key_count; ++note) {
            const DrumKey key = kits.key(note, kit);
            if (key.receives_note_on && key.tone > 0) {
                tones.insert(key.tone);
            }
        }
    }
    return tones;
}

} // namespace

BankBuild build_bank(const PatchDirectory& directory,
                     const DrumKitTable& kits,
                     const SampleSet& set,
                     const TvaChain& levels,
                     const TvfChain& filters,
                     const PitchChain& pitches,
                     const LfoEngine& lfos,
                     const BankOptions& options)
{
    BankBuild built;
    Bank& bank = built.bank;
    bank.name = options.name;
    bank.software = options.software;
    bank.comment = options.comment;
    bank.default_modulators = default_modulators(options.gs_modulators);

    // ── samples ──────────────────────────────────────────────────────────────
    //
    // The pool is re-laid rather than taken as-is: SampleSet packs runs back to back, and the
    // specification wants silence between them. Sample indices therefore differ from run indices.
    bank.pool.reserve(set.pool().size()
                      + (set.runs().size() * static_cast<std::size_t>(inter_sample_gap)));

    std::vector<int> sample_of_run(set.runs().size(), -1);
    for (std::size_t index = 0; index < set.runs().size(); ++index) {
        const SampleRun& run = set.runs()[index];
        const auto start = static_cast<std::uint32_t>(bank.pool.size());

        const auto first = set.pool().begin() + run.pool_offset;
        bank.pool.insert(bank.pool.end(), first, first + run.length);

        Sample sample;
        sample.name = "w" + std::to_string(run.wave);
        sample.start = start;
        sample.end = start + static_cast<std::uint32_t>(run.length);
        sample.sample_rate = static_cast<std::uint32_t>(options.sample_rate);
        sample.original_key = static_cast<std::uint8_t>(std::clamp(run.root_key, 0, 127));
        sample.correction = static_cast<std::int8_t>(std::clamp(run.fine_cents, -99, 99));

        if (run.loops()) {
            sample.loop_start = start + static_cast<std::uint32_t>(run.loop_start);
            sample.loop_end = start + static_cast<std::uint32_t>(run.loop_end);
        } else {
            // A non-looping sample still needs loop points inside its data; readers reject or
            // clamp a degenerate pair, and pointing both at the start is what other writers do.
            sample.loop_start = start;
            sample.loop_end = start + static_cast<std::uint32_t>(run.length);
        }

        sample_of_run[index] = static_cast<int>(bank.samples.size());
        bank.samples.push_back(std::move(sample));

        bank.pool.insert(bank.pool.end(), static_cast<std::size_t>(inter_sample_gap), 0.0F);
    }

    // ── instruments, one per present partial ─────────────────────────────────
    //
    // Partial-level rather than multisample-level, which costs about four times the instrument
    // zones. Drum kits are what decide it: a kit preset needs one zone per sounding key, up to 256
    // of them, and each has to point at an instrument that already carries the partial's
    // parameters. Hoisting those parameters into preset zones instead makes every kit re-emit them
    // 256 times over.
    const std::set<int> tones = reachable_tones(directory, kits);
    std::map<std::pair<int, int>, int> instrument_of_partial;

    for (const int tone_number : tones) {
        const std::optional<Tone> record = directory.tone(tone_number);
        if (!record || !record->is_defined()) {
            continue;
        }

        std::vector<ToneZone> zones;
        directory.tone_zones(tone_number, zones);

        for (std::size_t partial_index = 0; partial_index < record->partials().size();
             ++partial_index) {
            const PartialParameters& partial = record->partials()[partial_index];

            Instrument instrument;
            instrument.name = tidy(record->name()) + "#" + std::to_string(partial_index);
            int first_key = 60;
            bool have_first_key = false;

            const auto [velocity_low, velocity_high] = partial.velocity_window();
            const int tone_level = directory.tone_level(tone_number);

            for (const ToneZone& zone : zones) {
                if (zone.partial_index != static_cast<int>(partial_index)) {
                    continue;
                }
                const int run_index = set.run_for_wave(zone.wave);
                if (run_index < 0) {
                    continue;
                }

                if (!have_first_key) {
                    first_key = zone.key_low;
                    have_first_key = true;
                }

                Zone out;
                out.generators.push_back(
                    Generator::range(Gen::key_range, zone.key_low, zone.key_high));
                out.generators.push_back(
                    Generator::range(Gen::vel_range, velocity_low, velocity_high));

                // The amplitude envelope, fitted per key zone rather than per partial, because
                // its peak depends on the zone's own level and SF2 normalises the envelope to that
                // peak. Emitting one shape per partial and a separate attenuation would double
                // count wherever the peak segment sits below the base level.
                const int zone_level =
                    directory.zone_level(partial.multisample(), zone.key_low, partial.key_center());
                const SegmentEnvelope amplitude =
                    levels.create_envelope(partial, velocity_high, zone.key_low, zone_level,
                                           tone_level, options.sample_rate);
                const VolumeFit fit =
                    fit_volume(amplitude, options.sample_rate, options.fit_hold_seconds);

                built.worst_fit = std::max(built.worst_fit, fit.rms_error);
                built.fit_error_sum += fit.rms_error;
                ++built.fitted_zones;
                if (fit.moving_segments > 2) {
                    ++built.overflowed_zones;
                }

                // The peak the envelope actually reaches is the whole static level chain, so it
                // replaces the base level rather than sitting beside it.
                out.generators.push_back(
                    Generator::value(Gen::initial_attenuation, centibels(fit.peak)));

                emit_dahdsr(out.generators, fit.envelope);

                // ── the filter, and the one modulation envelope both it and pitch must share ──
                //
                // 81% of partials move the filter and 17% move the pitch, and SF2 has a single
                // modEnv reaching both. Where only one is active it gets the envelope outright.
                // Where both are -- 14% of partials -- the filter takes the shape, because it
                // carries the sustained gesture while the pitch envelope is usually a short
                // attack transient, and the pitch depth then rides the filter's timing.
                const int resonance = TvfChain::resonance_byte(partial);
                const bool filter_on = filters.tap(partial.filter_type()) != FilterTap::bypass;

                ModulationFit shape;
                if (filter_on) {
                    const TvfChain::Envelope cutoff = filters.create_envelope(
                        partial, velocity_high, zone.key_low, options.sample_rate);

                    std::vector<double> targets(cutoff.offsets.targets().begin(),
                                                cutoff.offsets.targets().end());
                    std::vector<double> ends;
                    ends.reserve(targets.size());
                    for (const auto end : cutoff.offsets.segment_ends()) {
                        ends.push_back(static_cast<double>(end) / options.sample_rate);
                    }

                    shape = fit_modulation(
                        targets, ends, 0.0, targets.back(),
                        static_cast<double>(cutoff.offsets.release_samples()) / options.sample_rate,
                        options.fit_hold_seconds);

                    const double low_hz = filters.cutoff_hz(
                        std::clamp(cutoff.base_cutoff + shape.low, 0.0, 32767.0), resonance,
                        options.sample_rate);
                    out.generators.push_back(
                        Generator::value(Gen::initial_filter_fc, absolute_cents(low_hz)));

                    // Reciprocal-Q: the neutral resonance byte 0x40 is exactly 1.0, and smaller
                    // bytes are more resonant. SF2 wants the peak height in centibels.
                    const double q = 64.0 / std::max(1, resonance);
                    const int q_cb = static_cast<int>(
                        std::lround(std::clamp(200.0 * std::log10(std::max(1.0, q)), 0.0, 960.0)));
                    if (q_cb > 0) {
                        out.generators.push_back(
                            Generator::value(Gen::initial_filter_q, q_cb));
                    }

                    if (shape.active) {
                        const double high_hz = filters.cutoff_hz(
                            std::clamp(cutoff.base_cutoff + shape.high, 0.0, 32767.0), resonance,
                            options.sample_rate);
                        const int depth = static_cast<int>(std::lround(
                            std::clamp(1200.0 * std::log2(std::max(1e-6, high_hz / low_hz)),
                                       -12000.0, 12000.0)));
                        if (depth != 0) {
                            out.generators.push_back(
                                Generator::value(Gen::mod_env_to_filter_fc, depth));
                        }
                        ++built.filter_envelopes;
                        built.filter_fit_sum += shape.rms_error;
                    }
                }

                // The pitch envelope. Its depth is in milli-semitones, which is a tenth of a cent.
                const std::optional<PitchEnvelope> pitch =
                    pitches.envelope_offsets(partial, zone.key_low, velocity_high);
                if (pitch) {
                    std::vector<double> targets(pitch->targets.begin(), pitch->targets.end());
                    std::vector<double> ends;
                    double accumulated = 0.0;
                    for (const double milliseconds : pitch->times) {
                        accumulated += milliseconds / 1000.0;
                        ends.push_back(accumulated);
                    }

                    const ModulationFit pitch_shape =
                        fit_modulation(targets, ends, pitch->start, pitch->release,
                                       pitch->release_ms / 1000.0, options.fit_hold_seconds);

                    if (pitch_shape.active) {
                        // Milli-semitones to cents: a semitone is 100 cents, so a milli-semitone
                        // is a tenth of one.
                        const int depth = static_cast<int>(
                            std::lround(std::clamp(pitch_shape.span() / 10.0, -12000.0, 12000.0)));
                        if (depth != 0) {
                            out.generators.push_back(
                                Generator::value(Gen::mod_env_to_pitch, depth));
                        }
                        ++built.pitch_envelopes;
                        built.pitch_fit_sum += pitch_shape.rms_error;

                        if (!shape.active) {
                            shape = pitch_shape;
                        } else {
                            ++built.shared_mod_envelopes;
                        }
                    }
                }

                if (shape.active) {
                    emit_mod_dahdsr(out.generators, shape.envelope);
                }

                // ── the two LFOs ─────────────────────────────────────────────
                //
                // LFO1 is tone-common and takes the part's vibrato modifiers, so it is the vibrato
                // LFO; LFO2 is per-partial and becomes the modulation LFO. Both destinations are
                // reachable for both, but only because of the extended generators: standard SF2
                // gives the vibrato LFO pitch alone, so on a conforming reader LFO1 degrades to
                // vibrato and its filter and amplitude depths are dropped.
                const auto [lfo1, lfo2] = lfos.configure(tone_number, partial);

                const auto emit_lfo = [&](const LfoConfig& config, bool vibrato) {
                    const double delay = lfo_delay_seconds(config.delay_rate);
                    if (delay < 0.0 || config.increment <= 0) {
                        return;
                    }
                    const bool moves = config.pitch_depth != 0 || config.tvf_depth != 0
                                       || config.tva_depth != 0;
                    if (!moves) {
                        return;
                    }

                    if (LfoEngine::is_random(config.waveform)) {
                        // Waveforms 1 to 3 redraw when the phase wraps rather than being functions
                        // of it. SF2's LFOs are triangles and cannot be anything else, so these are
                        // emitted as triangles and counted as a known loss.
                        ++built.random_lfos;
                    }

                    out.generators.push_back(Generator::value(
                        vibrato ? Gen::freq_vib_lfo : Gen::freq_mod_lfo,
                        absolute_cents(lfo_hz(config.increment))));
                    out.generators.push_back(Generator::value(
                        vibrato ? Gen::delay_vib_lfo : Gen::delay_mod_lfo, to_timecents(delay)));

                    if (config.pitch_depth != 0) {
                        // Milli-semitones to cents.
                        const int cents = static_cast<int>(
                            std::lround(std::clamp(config.pitch_depth / 10.0, -12000.0, 12000.0)));
                        if (cents != 0) {
                            out.generators.push_back(Generator::value(
                                vibrato ? Gen::vib_lfo_to_pitch : Gen::mod_lfo_to_pitch, cents));
                        }
                    }

                    if (config.tvf_depth != 0 && filter_on) {
                        const double centre = filters.cutoff_hz(
                            std::clamp(static_cast<double>(partial.cutoff_base() * 0x100), 1.0,
                                       32767.0),
                            resonance, options.sample_rate);
                        const double swung = filters.cutoff_hz(
                            std::clamp(static_cast<double>((partial.cutoff_base() * 0x100)
                                                           + config.tvf_depth),
                                       1.0, 32767.0),
                            resonance, options.sample_rate);
                        const int cents = static_cast<int>(std::lround(std::clamp(
                            1200.0 * std::log2(std::max(1e-6, swung / centre)), -12000.0,
                            12000.0)));
                        if (cents != 0) {
                            out.generators.push_back(Generator::value(
                                vibrato ? Gen::vib_lfo_to_filter_fc : Gen::mod_lfo_to_filter_fc,
                                cents));
                        }
                    }

                    if (config.tva_depth != 0) {
                        // The engine's amplitude depth is a fraction of 0x7f00 applied
                        // multiplicatively. The modulation LFO has a centibel destination in
                        // standard SF2; the vibrato LFO only has the extended tenths-of-a-percent
                        // one, which is unipolar where the engine's swing is not, so it takes twice
                        // the fraction to cover the same peak-to-peak.
                        const double fraction =
                            std::abs(static_cast<double>(config.tva_depth)) / 32512.0;
                        if (vibrato) {
                            const int depth = static_cast<int>(
                                std::lround(std::clamp(fraction * 2000.0, 0.0, 1000.0)));
                            if (depth > 0) {
                                out.generators.push_back(Generator::value(
                                    Gen::vib_lfo_amplitude_depth, depth));
                            }
                        } else {
                            const int centibels = static_cast<int>(std::lround(
                                std::clamp(200.0 * std::log10(1.0 + fraction), 0.0, 960.0)));
                            if (centibels > 0) {
                                out.generators.push_back(
                                    Generator::value(Gen::mod_lfo_to_volume, centibels));
                            }
                        }
                    }
                    ++built.lfos_emitted;
                };

                emit_lfo(lfo1, /*vibrato=*/true);
                emit_lfo(lfo2, /*vibrato=*/false);

                if (partial.pan() != 0x40) {
                    // SF2 pan is tenths of a percent either side of centre; the partial's is a
                    // 0x40-centred byte.
                    const int pan = ((partial.pan() - 0x40) * 1000) / 64;
                    out.generators.push_back(
                        Generator::value(Gen::pan, std::clamp(pan, -500, 500)));
                }

                const SampleRun& run = set.runs()[static_cast<std::size_t>(run_index)];
                out.generators.push_back(Generator::value(
                    Gen::sample_modes,
                    static_cast<int>(run.loops() ? LoopMode::loop : LoopMode::no_loop)));

                // sampleID must be the last generator in a zone.
                out.generators.push_back(Generator::value(
                    Gen::sample_id, sample_of_run[static_cast<std::size_t>(run_index)]));

                instrument.zones.push_back(std::move(out));
            }

            if (instrument.zones.empty()) {
                continue;
            }

            // ── the partial's own modulators, in a global zone ───────────────
            //
            // A zone modulator whose source, destination and amount-source match a bank default
            // *replaces* it rather than adding to it, and the reader merges an instrument's global
            // zone the same way (`ss_preset_get_synthesis_data`: instrument zone, then unique
            // global, then unique defaults). So per-partial behaviour that contradicts a default
            // belongs here, once per instrument rather than once per key zone.
            Zone global;

            // Velocity. The default set applies a uniform 960 cB concave response to everything,
            // which this engine does not do: a partial crossfades between its own two edge levels
            // across its own velocity window, and some partials run the other way entirely --
            // louder at the bottom of the window than the top. Replacing the default with the
            // measured span keeps both the depth and the direction.
            //
            // It stays an approximation. The modulator's curve spans the whole 0-127 range while
            // the partial's crossfade spans only its window, so a narrow window is under-served.
            {
                const auto [velocity_low, velocity_high] = partial.velocity_window();
                const int tone_level = directory.tone_level(tone_number);
                const int key = instrument.zones.empty() ? 60 : first_key;

                const std::optional<int> low_level = levels.partial_level(partial, velocity_low);
                const std::optional<int> high_level = levels.partial_level(partial, velocity_high);
                if (low_level && high_level) {
                    const int zone_level = directory.zone_level(partial.multisample(), key,
                                                                partial.key_center());
                    const double low_gain = levels.amp_of(
                        levels.base_level(partial, *low_level, key, zone_level, tone_level));
                    const double high_gain = levels.amp_of(
                        levels.base_level(partial, *high_level, key, zone_level, tone_level));
                    const int span = centibels(low_gain) - centibels(high_gain);

                    global.modulators.push_back(Modulator{velocity_attenuation_source(),
                                                          Gen::initial_attenuation,
                                                          static_cast<std::int16_t>(
                                                              std::clamp(span, -960, 960)),
                                                          0, 0});
                    if (span < 0) {
                        ++built.inverted_velocity_partials;
                    }
                }
            }

            // Half-damper. Only the 57 piano tones respond to a partly-pressed pedal at all; every
            // other tone quantises CC#64 to fully up or fully down before it can reach the release
            // ramp. A bank-wide default would lengthen the whole library's release.
            if (directory.half_damper(tone_number)) {
                global.modulators.push_back(
                    Modulator{modulator_source(0, false, false, true, 64), Gen::release_vol_env,
                              1200, 0, 0});
                ++built.half_damper_instruments;
            }

            // CC#72/73/75 reach the filter envelope as well as the amplitude one, but only on
            // partials that opt in through bit 4 of block byte 0x0e. `tvf_compute_env_rates` zeroes
            // the bias outright when the bit is clear, so on the rest those controllers move the
            // amplitude envelope alone -- which is what the bank defaults already do.
            if (TvfChain::responds_to_env_modifiers(partial)) {
                global.modulators.push_back(
                    Modulator{modulator_source(0, true, false, true, 73), Gen::attack_mod_env,
                              6000, 0, 0});
                global.modulators.push_back(
                    Modulator{modulator_source(0, true, false, true, 75), Gen::decay_mod_env, 3600,
                              0, 0});
                global.modulators.push_back(
                    Modulator{modulator_source(0, true, false, true, 72), Gen::release_mod_env,
                              3600, 0, 0});
                ++built.env_modifier_partials;
            }

            if (!global.modulators.empty()) {
                // A global zone is one carrying no `sampleID`, and it must come first.
                instrument.zones.insert(instrument.zones.begin(), std::move(global));
            }

            instrument_of_partial.emplace(
                std::pair{tone_number, static_cast<int>(partial_index)},
                static_cast<int>(bank.instruments.size()));
            bank.instruments.push_back(std::move(instrument));
        }
    }

    // ── presets: one per melodic tone, at its ROM-aligned slot ───────────────
    for (const int tone_number : tones) {
        const std::optional<Tone> record = directory.tone(tone_number);
        if (!record || !record->is_defined()) {
            continue;
        }

        Preset preset;
        preset.name = tidy(record->name());
        preset.program = static_cast<std::uint16_t>(tone_number & 0x7F);
        // The packed bank word: MSB in the low seven bits, LSB in the high byte. The tone's own
        // number is the address, so a preset is a stable name for a tone across all five maps and
        // the sflist files can point at it without knowing how the bank was built.
        preset.bank = static_cast<std::uint16_t>((tone_number >> 7) << 8);

        for (std::size_t partial_index = 0; partial_index < record->partials().size();
             ++partial_index) {
            const auto found =
                instrument_of_partial.find({tone_number, static_cast<int>(partial_index)});
            if (found == instrument_of_partial.end()) {
                continue;
            }
            Zone zone;
            zone.generators.push_back(Generator::value(Gen::instrument, found->second));
            preset.zones.push_back(std::move(zone));
        }

        if (preset.zones.empty()) {
            continue;
        }
        ++built.melodic_presets;
        bank.presets.push_back(std::move(preset));
    }

    // ── presets: one per drum kit ────────────────────────────────────────────
    for (int kit = 0; kit < kits.kit_count(); ++kit) {
        Preset preset;
        preset.name = tidy(kits.kit_name(kit));
        if (preset.name.empty()) {
            preset.name = "Kit " + std::to_string(kit);
        }
        preset.program = static_cast<std::uint16_t>(kit & 0x7F);
        // Bit 7 of the low byte is the percussion flag the reader tests.
        preset.bank = static_cast<std::uint16_t>(0x80 | ((kit >> 7) << 8));

        for (int note = 0; note < DrumKitTable::key_count; ++note) {
            const DrumKey key = kits.key(note, kit);
            if (!key.receives_note_on || key.tone <= 0) {
                continue;
            }
            const std::optional<Tone> record = directory.tone(key.tone);
            if (!record || !record->is_defined()) {
                continue;
            }

            for (std::size_t partial_index = 0; partial_index < record->partials().size();
                 ++partial_index) {
                const auto found =
                    instrument_of_partial.find({key.tone, static_cast<int>(partial_index)});
                if (found == instrument_of_partial.end()) {
                    continue;
                }

                Zone zone;
                zone.generators.push_back(Generator::range(Gen::key_range, note, note));

                if (key.level != 127) {
                    const double gain = (key.level / 127.0) * (key.level / 127.0);
                    zone.generators.push_back(
                        Generator::value(Gen::initial_attenuation, centibels(gain)));
                }
                if (key.pan != 0x40) {
                    const int pan = ((key.pan - 0x40) * 1000) / 64;
                    zone.generators.push_back(
                        Generator::value(Gen::pan, std::clamp(pan, -500, 500)));
                }
                if (key.group != 0) {
                    zone.generators.push_back(
                        Generator::value(Gen::exclusive_class, key.group));
                }

                // instrument must be the last generator in a preset zone.
                zone.generators.push_back(Generator::value(Gen::instrument, found->second));
                preset.zones.push_back(std::move(zone));
            }
        }

        if (preset.zones.empty()) {
            continue;
        }
        ++built.drum_presets;
        bank.presets.push_back(std::move(preset));
    }

    return built;
}

} // namespace ts::sf2
