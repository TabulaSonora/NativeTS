#include "tabulasonora/patch_directory.hpp"

#include "dsp/fixed.hpp"

#include <algorithm>
#include <stdexcept>

namespace ts {
namespace {

/// Reads a signed 16-bit field. The multisample tables use -1 to mean "no entry here", so the
/// signedness is load-bearing: read unsigned, an empty zone becomes wave 65535.
[[nodiscard]] int read_i16(std::span<const std::uint8_t> record, int offset) noexcept
{
    return fx::read_i16le(record.data() + offset);
}

} // namespace

PatchDirectory::PatchDirectory(const TableSet& tables)
    : tables_(&tables),
      tone_(tables.tone()),
      multisample_(tables.multisample()),
      wavedesc_(tables.wavedesc()),
      layered_(tables.layered())
{
    tone_count_ = static_cast<int>(tone_.size()) / Tone::stride;
    multisample_count_ = static_cast<int>(multisample_.size()) / multisample_stride;
    wave_count_ = static_cast<int>(wavedesc_.size()) / WaveDescriptor::stride;
    alternate_count_ = static_cast<int>(layered_.size()) / alternate_stride;
}

std::optional<WaveDescriptor> PatchDirectory::wave(int wave_number) const
{
    if (wave_number < 0 || wave_number >= wave_count_) {
        return std::nullopt;
    }
    const auto offset = static_cast<std::size_t>(wave_number) * WaveDescriptor::stride;
    return WaveDescriptor::parse(wavedesc_.subspan(offset, WaveDescriptor::stride));
}

std::optional<Tone> PatchDirectory::tone(int tone_number) const
{
    if (tone_number < 0 || tone_number >= tone_count_) {
        return std::nullopt;
    }
    return Tone::read(tone_, tone_number);
}

int PatchDirectory::tone_level(int tone_number) const
{
    return tone_[(static_cast<std::size_t>(tone_number) * Tone::stride) + 0x0C];
}

bool PatchDirectory::half_damper(int tone_number) const
{
    return tone_number >= 0 && tone_number < tone_count_
           && (tone_[(static_cast<std::size_t>(tone_number) * Tone::stride) + 0x0D] & 4) != 0;
}

PartialParameters PatchDirectory::partial_by_slot(int tone_number, int slot) const
{
    if (slot < 0 || slot >= Tone::partial_slots) {
        throw std::out_of_range("A melodic tone has two partial slots.");
    }

    const std::size_t offset = (static_cast<std::size_t>(tone_number) * Tone::stride)
                               + Tone::header_size
                               + (static_cast<std::size_t>(slot) * PartialParameters::stride);
    return PartialParameters{tone_.data() + offset};
}

std::optional<int> PatchDirectory::multisample_wave(int multisample, int transposed_key) const
{
    if (multisample < 0 || multisample >= multisample_count_) {
        return std::nullopt;
    }

    const auto record = multisample_.subspan(
        static_cast<std::size_t>(multisample) * multisample_stride, multisample_stride);

    int zone = 0x1F;
    for (int z = 0; z < 0x20; ++z) {
        const int bound = record[static_cast<std::size_t>(0x0C + z)];
        if (transposed_key <= bound || bound >= 0x7F) {
            zone = z;
            break;
        }
    }

    int wave_number = read_i16(record, 0x2C + (zone * 2));

    // An empty bottom zone means the note is below the sampled range, and the partial is silent --
    // it must not fall through to the neighbouring wave.
    if (wave_number < 0 && zone == 0) {
        return std::nullopt;
    }

    if (wave_number < 0) {
        wave_number = read_i16(record, 0x2E + (zone * 2));
    }
    if (wave_number < 0) {
        wave_number = read_i16(record, 0x6A);
    }

    return wave_number >= 0 ? std::optional{wave_number} : std::nullopt;
}

int PatchDirectory::zone_level(int multisample, int key, int key_center) const
{
    if (multisample < 0 || multisample >= multisample_count_) {
        return 127;
    }

    const auto record = multisample_.subspan(
        static_cast<std::size_t>(multisample) * multisample_stride, multisample_stride);
    const int transposed_key = std::clamp(key + (0x40 - key_center), 0, 0x7F);

    // 0x2b is the last key bound; past it, the tail entry applies.
    if (record[0x2B] < transposed_key) {
        return record[0x8B];
    }

    int z = 0;
    while (record[static_cast<std::size_t>(0x0C + z)] < transposed_key) {
        ++z;
    }

    if (read_i16(record, 0x2C + (z * 2)) < 0) {
        // No direct wave: the level comes from the velocity-alternate plane instead.
        if (z == 0) {
            return record[static_cast<std::size_t>(0x6D + z)];
        }
        return record[static_cast<std::size_t>(0x0C + z)] == 0x7F
                   ? record[static_cast<std::size_t>(0x6B + z)]
                   : 127;
    }

    return record[static_cast<std::size_t>(0x6C + z)];
}

std::vector<MultisampleZone> PatchDirectory::multisample_zones(int multisample) const
{
    std::vector<MultisampleZone> zones;
    if (multisample < 0 || multisample >= multisample_count_) {
        return zones;
    }

    const auto record = multisample_.subspan(
        static_cast<std::size_t>(multisample) * multisample_stride, multisample_stride);
    zones.reserve(8);
    int previous = 0;

    for (int z = 0; z < 0x20; ++z) {
        const int bound = record[static_cast<std::size_t>(0x0C + z)];
        int wave_number = read_i16(record, 0x2C + (z * 2));
        if (wave_number < 0) {
            wave_number = read_i16(record, 0x2E + (z * 2));
        }

        if (wave_number >= 0) {
            zones.push_back(MultisampleZone{previous, bound, wave_number});
        }

        previous = bound + 1;
        if (bound >= 0x7F) {
            break;
        }
    }

    return zones;
}

std::optional<std::string> PatchDirectory::tone_zones(int tone_number,
                                                      std::vector<ToneZone>& zones) const
{
    zones.clear();

    const std::optional<Tone> record = tone(tone_number);
    if (!record || !record->is_defined()) {
        return std::nullopt;
    }

    for (std::size_t partial_index = 0; partial_index < record->partials().size();
         ++partial_index) {
        const PartialParameters& partial = record->partials()[partial_index];
        const int key_center = partial.key_center();
        const auto [velocity_low, velocity_high] = partial.velocity_window();

        for (const MultisampleZone& zone : multisample_zones(partial.multisample())) {
            const int note_low = std::max(0, zone.key_low + key_center - 0x40);
            const int note_high = std::min(127, zone.key_high + key_center - 0x40);
            if (note_high < note_low) {
                continue;
            }

            const std::optional<WaveDescriptor> descriptor = wave(zone.wave);
            if (!descriptor) {
                continue;
            }

            zones.push_back(ToneZone{
                .partial_index = static_cast<int>(partial_index),
                .velocity_low = velocity_low,
                .velocity_high = velocity_high,
                .key_low = note_low,
                .key_high = note_high,
                .wave = zone.wave,
                .descriptor = *descriptor,
            });
        }
    }

    return record->name();
}

const std::vector<std::pair<std::string, int>>& tone_map_choices() noexcept
{
    // Ordered as a help string wants to read them, oldest module first, with XG last because it is
    // not a vintage of the same instrument.
    static const std::vector<std::pair<std::string, int>> choices{
        {"sc55", static_cast<int>(ToneMap::sc55)},
        {"sc88", static_cast<int>(ToneMap::sc88)},
        {"sc88pro", static_cast<int>(ToneMap::sc88pro)},
        {"sc8820", static_cast<int>(ToneMap::sc8820)},
        {"xg", static_cast<int>(ToneMap::xg)},
    };
    return choices;
}

std::string_view tone_map_name(ToneMap map) noexcept
{
    for (const auto& [name, value] : tone_map_choices()) {
        if (value == static_cast<int>(map)) {
            return name;
        }
    }
    return {};
}

std::optional<int> PatchDirectory::lut3_raw(int program, ToneMap map, int bank) const
{
    const auto map_index = static_cast<std::size_t>(map);
    const auto lut1 = tables_->dir_lut1();
    if (static_cast<int>(map) < 0 || map_index >= lut1.size()) {
        return std::nullopt;
    }

    const int level1 = lut1[map_index];
    if (level1 == 0xFF) {
        return std::nullopt;
    }

    const auto lut2 = tables_->dir_lut2();
    const auto index2 = static_cast<std::size_t>((level1 * 0x80) + bank);
    if (index2 >= lut2.size() || lut2[index2] == 0xFF) {
        return std::nullopt;
    }

    const auto lut3 = tables_->dir_lut3();
    const auto index3 = static_cast<std::size_t>((lut2[index2] * 0x80) + program);
    if (index3 >= lut3.size()) {
        return std::nullopt;
    }

    // Unsigned on purpose: the 0x8000 bit marks a tone reachable only through an
    // alternate-articulation entry, not a missing one.
    return lut3[index3];
}

std::optional<int> PatchDirectory::lut3_resolved(int program, ToneMap map, int bank) const
{
    // Two banks redirect instead of resolving. `program_resolve_tone` @`180069200` tests the lookup
    // bank before any of the three levels run, and 0x40 and 0x41 send it through an indirection
    // table indexed by program whose first three planes *replace* the map, bank and program the
    // lookup then uses. The shipped data makes 0x40 mean "map 2, bank 0" and 0x41 "map 1, bank 0",
    // both keeping the program -- the SC-88 and SC-55 compatibility banks, reachable from any map.
    //
    // XG is what makes them matter: its variations hang off the bank LSB, so a file selecting LSB
    // 64 or 65 is asking for an older Sound Canvas's voice. Without this it got whatever the
    // current map happened to hold there, which for the Cave Story XG set was a two-partial tone
    // where the module plays one -- five decibels of it.
    //
    // Here and not in `lut3_raw`, which is a raw table read and has to stay one: the bank-count
    // gate gets each vintage's *native* bank set from it, and a vintage does not natively define
    // the two banks it merely redirects through.
    if (bank == indirect_bank_88 || bank == indirect_bank_55) {
        const auto table = bank == indirect_bank_88 ? tables_->tone_indirect_bank64()
                                                    : tables_->tone_indirect_bank65();
        constexpr std::size_t plane = 0x80;
        const auto at = static_cast<std::size_t>(program);
        if (program < 0 || at >= plane || table.size() < 3 * plane) {
            return std::nullopt;
        }

        const int substituted_bank = table[plane + at];
        // The shipped tables substitute bank 0, so this recurses once. The guard is against data
        // that says otherwise rather than against anything observed.
        if (substituted_bank == indirect_bank_88 || substituted_bank == indirect_bank_55) {
            return std::nullopt;
        }
        return lut3_resolved(static_cast<int>(table[(2 * plane) + at]),
                             static_cast<ToneMap>(table[at]),
                             substituted_bank);
    }

    std::optional<int> raw = lut3_raw(program, map, bank);
    if (bank != 0 && (!raw || *raw == unassigned)) {
        raw = lut3_raw(program, map, 0);
    }
    return raw;
}

std::optional<AlternateEntry> PatchDirectory::alternate(int index) const
{
    if (index < 0 || index >= alternate_count_) {
        return std::nullopt;
    }

    const auto record =
        layered_.subspan(static_cast<std::size_t>(index) * alternate_stride, alternate_stride);

    std::string name;
    name.reserve(10);
    for (std::size_t i = 0; i < 10; ++i) {
        name.push_back(static_cast<char>(record[i]));
    }
    const auto last = name.find_last_not_of(std::string_view{" \0", 2});
    name.erase(last == std::string::npos ? 0 : last + 1);

    return AlternateEntry{
        .name = std::move(name),
        .threshold = record[0x0E],
        .primary = ProgramReference{record[0x10], record[0x11], record[0x12]},
        .alternate = ProgramReference{record[0x14], record[0x15], record[0x16]},
    };
}

std::vector<int> PatchDirectory::program_tones(int program, ToneMap map, int bank) const
{
    const std::optional<int> raw = lut3_resolved(program, map, bank);
    if (!raw || *raw == unassigned || *raw >= indirect_only_flag) {
        return {};
    }

    if (*raw < alternate_space_start) {
        return {*raw};
    }

    const std::optional<AlternateEntry> entry = alternate(*raw - alternate_space_start);
    if (!entry) {
        return {};
    }

    const int tone_number = dereference(entry->primary);
    return tone_number >= 0 ? std::vector<int>{tone_number} : std::vector<int>{};
}

int PatchDirectory::alternate_tone(int program, ToneMap map, int bank) const
{
    const std::optional<int> raw = lut3_resolved(program, map, bank);
    if (!raw || *raw == unassigned || *raw >= indirect_only_flag || *raw < alternate_space_start) {
        return -1;
    }

    const std::optional<AlternateEntry> entry = alternate(*raw - alternate_space_start);
    return entry ? dereference(entry->alternate) : -1;
}

int PatchDirectory::program_to_tone(int program, ToneMap map, int bank) const
{
    const std::vector<int> tones = program_tones(program, map, bank);
    return tones.empty() ? -1 : tones.front();
}

ResolvedTone PatchDirectory::resolve(int tone_number, int note, int velocity) const
{
    const std::optional<Tone> record = tone(tone_number);
    if (!record || !record->is_defined()) {
        return ResolvedTone{"(none)", {}};
    }

    std::vector<ResolvedPartial> sounding;
    sounding.reserve(2);

    for (std::size_t partial_index = 0; partial_index < record->partials().size();
         ++partial_index) {
        const PartialParameters& partial = record->partials()[partial_index];
        if (!partial.accepts_velocity(velocity)) {
            continue;
        }

        const std::optional<int> wave_number =
            multisample_wave(partial.multisample(), partial.transposed_key(note));
        if (!wave_number) {
            continue;
        }

        const std::optional<WaveDescriptor> descriptor = wave(*wave_number);
        if (!descriptor) {
            continue;
        }

        sounding.push_back(ResolvedPartial{
            .partial_index = static_cast<int>(partial_index),
            .wave = *wave_number,
            .descriptor = *descriptor,
        });
    }

    return ResolvedTone{record->name(), std::move(sounding)};
}

ResolvedTone
PatchDirectory::resolve_midi(int program, int note, int velocity, ToneMap map, int bank) const
{
    const int tone_number = program_to_tone(program, map, bank);
    return tone_number < 0 ? ResolvedTone{"(unassigned)", {}}
                           : resolve(tone_number, note, velocity);
}

int PatchDirectory::dereference(const ProgramReference& reference) const
{
    const std::optional<int> raw =
        lut3_raw(reference.program, static_cast<ToneMap>(reference.map), reference.bank);
    if (!raw || *raw == unassigned) {
        return -1;
    }

    // Strip the indirect-only marker; the program-change handler does the same.
    const int tone_number = *raw & 0x7FFF;
    return tone_number < melodic_space_end ? tone_number : -1;
}

} // namespace ts
