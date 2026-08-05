#include "tabulasonora/soundfont_bank.hpp"

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
                     const BankOptions& options)
{
    BankBuild built;
    Bank& bank = built.bank;
    bank.name = options.name;
    bank.software = options.software;
    bank.comment = options.comment;

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

                Zone out;
                out.generators.push_back(
                    Generator::range(Gen::key_range, zone.key_low, zone.key_high));
                out.generators.push_back(
                    Generator::range(Gen::vel_range, velocity_low, velocity_high));

                // The level chain's four attenuations collapse into one static value. This is
                // provisional: it is taken at the top of the partial's velocity window, so the
                // velocity-dependent part is frozen there and the default velocity modulator
                // supplies the rest. The envelope pass replaces it.
                const int zone_level =
                    directory.zone_level(partial.multisample(), zone.key_low, partial.key_center());
                const std::optional<int> partial_level =
                    levels.partial_level(partial, velocity_high);
                if (partial_level) {
                    const int level16 = levels.base_level(partial, *partial_level,
                                                                zone.key_low, zone_level,
                                                                tone_level);
                    out.generators.push_back(Generator::value(
                        Gen::initial_attenuation, centibels(levels.amp_of(level16))));
                }

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
