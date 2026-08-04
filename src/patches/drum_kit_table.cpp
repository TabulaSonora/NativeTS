#include "tabulasonora/drum_kit_table.hpp"

#include "dsp/fixed.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ts {

DrumKitTable::DrumKitTable(const RomImage& rom)
{
    kit_base_ = rom.manifest().region("drum_kit_records").file_offset;
    bank_row_ = rom.read(rom.manifest().region("drum_bank_row").file_offset, 256);
    program_map_ = rom.read(rom.manifest().region("drum_prog_map").file_offset,
                            static_cast<std::size_t>(map_row_count) * 0x80);

    const std::vector<std::uint8_t> index_bytes =
        rom.read(kit_index_offset, kit_index_.size() * sizeof(std::uint16_t));
    for (std::size_t i = 0; i < kit_index_.size(); ++i) {
        kit_index_[i] = fx::read_u16le(index_bytes.data() + (i * 2));
    }

    // Read only as many records as the index can actually reach.
    int highest = 0;
    for (int row = 0; row < map_row_count; ++row) {
        for (int program = 0; program < 0x80; ++program) {
            const int level2 = program_map_[static_cast<std::size_t>((row * 0x80) + program)];
            if (level2 != 0xFF) {
                highest = std::max(highest,
                                   static_cast<int>(kit_index_[static_cast<std::size_t>(level2)]));
            }
        }
    }

    kit_count_ = highest + 1;
    kits_ = rom.read(kit_base_, static_cast<std::size_t>(kit_count_) * kit_stride);
}

std::optional<int> DrumKitTable::row_for_map(ToneMap map) noexcept
{
    switch (map) {
    case ToneMap::sc55:
        return 3;
    case ToneMap::sc88:
        return 2;
    case ToneMap::sc88pro:
        return 1;
    case ToneMap::sc8820:
        return 0;
    case ToneMap::xg:
        return 4;
    }
    return std::nullopt;
}

int DrumKitTable::map_row(int internal_bank) const noexcept
{
    return bank_row_[static_cast<std::size_t>(internal_bank & 0xFF)];
}

std::optional<int> DrumKitTable::kit_for_program(int program, int row) const noexcept
{
    if (row < 0 || row >= map_row_count) {
        return std::nullopt;
    }

    const int level2 = program_map_[static_cast<std::size_t>((row * 0x80) + (program & 0x7F))];
    return level2 == 0xFF
               ? std::nullopt
               : std::optional{static_cast<int>(kit_index_[static_cast<std::size_t>(level2)])};
}

std::string DrumKitTable::kit_name(int kit) const
{
    const auto offset = static_cast<std::size_t>(kit) * kit_stride + name_plane;
    if (kit < 0 || offset + name_length > kits_.size()) {
        return {};
    }

    std::string name(reinterpret_cast<const char*>(kits_.data() + offset), name_length);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) {
        name.pop_back();
    }
    return name;
}

DrumKey DrumKitTable::key(int note, int kit) const
{
    if (note < 0 || note >= key_count) {
        throw std::out_of_range("A drum kit has 128 keys.");
    }

    const auto offset = static_cast<std::size_t>(kit) * kit_stride;
    return DrumKey{
        .tone = fx::read_u16le(kits_.data() + offset + tone_plane
                               + (static_cast<std::size_t>(note) * 2)),
        .level = kits_[offset + level_plane + static_cast<std::size_t>(note)],
        .pitch = kits_[offset + pitch_plane + static_cast<std::size_t>(note)],
        .group = kits_[offset + group_plane + static_cast<std::size_t>(note)],
        .pan = kits_[offset + pan_plane + static_cast<std::size_t>(note)],
        .receives_note_off =
            (kits_[offset + receive_plane + static_cast<std::size_t>(note)] & 1U) != 0,
        .receives_note_on =
            (kits_[offset + receive_plane + static_cast<std::size_t>(note)] & 0x10U) != 0,
    };
}

double DrumKitTable::coarse_pitch_ratio(int pitch) noexcept
{
    return std::pow(2.0, (pitch - 60) / 24.0);
}

} // namespace ts
