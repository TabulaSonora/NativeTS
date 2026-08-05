#include "tabulasonora/soundfont_sflist.hpp"

#include <nlohmann/json.hpp>

namespace ts::sf2 {
namespace {

using nlohmann::json;

/// The XG lookup bank the module substitutes for bank MSB 64, the SFX voice column.
constexpr int xg_sfx_lookup_bank = 0x7D;

/// The MIDI bank word a vintage's variation lands on, packed as the reader unpacks it.
///
/// GS carries the variation in the MSB; XG carries it in the LSB. The packed word is
/// `msb | (lsb << 8)`, which is what `sflist` compares against.
[[nodiscard]] int destination_bank(ToneMap map, int variation) noexcept
{
    if (map == ToneMap::xg) {
        return (variation & 0x7F) << 8;
    }
    return variation & 0x7F;
}

/// The slot the exporter wrote a melodic tone to.
[[nodiscard]] json melodic_source(int tone)
{
    return json{{"bank", (tone >> 7) << 8}, {"program", tone & 0x7F}};
}

/// The slot the exporter wrote a drum kit to, including the percussion flag the reader adds.
[[nodiscard]] json drum_source(int kit)
{
    return json{{"bank", ((kit >> 7) << 8) | 128}, {"program", kit & 0x7F}};
}

[[nodiscard]] json mapping(json source, json destination)
{
    return json{{"source", std::move(source)}, {"destination", std::move(destination)}};
}

} // namespace

std::string build_sflist(const PatchDirectory& directory,
                         const DrumKitTable& kits,
                         ToneMap map,
                         const SflistOptions& options,
                         SflistReport& report)
{
    report = SflistReport{};
    json mappings = json::array();

    for (int bank = 0; bank < 128; ++bank) {
        // An absent bank reads back as nothing at all, where a present one yields a value for
        // every program -- possibly the unassigned marker. Testing the value rather than the
        // presence would treat the whole map as one bank, because the capital-tone fallback
        // answers for banks the vintage never had.
        if (!directory.lut3_raw(0, map, bank)) {
            continue;
        }
        ++report.banks;

        for (int program = 0; program < 128; ++program) {
            const bool own = directory.lut3_raw(program, map, bank).value_or(
                                 PatchDirectory::unassigned)
                             != PatchDirectory::unassigned;
            if (!own && !options.capital_fallback) {
                continue;
            }

            const std::vector<int> tones = directory.program_tones(program, map, bank);
            if (tones.empty()) {
                continue;
            }

            mappings.push_back(mapping(
                melodic_source(tones.front()),
                json{{"bank", destination_bank(map, bank)}, {"program", program}}));

            ++report.melodic_mappings;
            if (!own) {
                ++report.fallback_mappings;
            }
        }
    }

    // The XG SFX voice column. Bank MSB 64 is not a variation of the XG map but a column of it, and
    // the module reaches it by substituting lookup bank 0x7d whatever the LSB says -- so it is one
    // destination rather than 128, and it has to be emitted from the lookup bank the substitution
    // names rather than from the MSB itself.
    if (map == ToneMap::xg && directory.lut3_raw(0, map, xg_sfx_lookup_bank)) {
        for (int program = 0; program < 128; ++program) {
            const std::vector<int> tones =
                directory.program_tones(program, map, xg_sfx_lookup_bank);
            if (tones.empty()) {
                continue;
            }
            mappings.push_back(
                mapping(melodic_source(tones.front()), json{{"bank", 64}, {"program", program}}));
            ++report.melodic_mappings;
        }
    }

    if (options.drums) {
        const std::optional<int> row = DrumKitTable::row_for_map(map);
        if (row) {
            for (int program = 0; program < 128; ++program) {
                const std::optional<int> kit = kits.kit_for_program(program, *row);
                if (!kit) {
                    continue;
                }
                // The destination bank is zero: the percussion flag rides along from the source
                // preset, so the kit lands on the standard drum bank rather than needing one.
                mappings.push_back(
                    mapping(drum_source(*kit), json{{"bank", 0}, {"program", program}}));
                ++report.drum_mappings;
            }
        }
    }

    json bank_entry;
    bank_entry["fileName"] = options.file_name;
    bank_entry["patchMappings"] = std::move(mappings);

    json root;
    root["soundFonts"] = json::array({std::move(bank_entry)});
    return root.dump(2) + "\n";
}

} // namespace ts::sf2
