#include "tabulasonora/patch_directory.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ts;
namespace fs = std::filesystem;

namespace {

constexpr std::array<std::pair<ToneMap, const char*>, 4> all_maps{{
    {ToneMap::sc55, "Sc55"},
    {ToneMap::sc88, "Sc88"},
    {ToneMap::sc88pro, "Sc88Pro"},
    {ToneMap::sc8820, "Sc8820"},
}};

[[nodiscard]] TableSet load_tables()
{
    return TableSet::from_cache_directory(testdata::require_tables());
}

} // namespace

TEST_CASE("every slot resolves the same as an independent resolver", "[patch][sccore][gate]")
{
    // The Phase 3 gate: all four vintages x 128 banks x 128 programs, against a Python resolver
    // written from the notes rather than translated from this code. Both the tone number and the
    // name it carries are compared, so a lookup landing on a real-but-wrong tone is caught.
    const fs::path fixture_path =
        testdata::repository_root() / "fixtures" / "patch_resolution.json";
    if (!fs::exists(fixture_path)) {
        SKIP(
            "No patch-resolution fixture. Generate it with:\n"
            "  python3 tools/dump_patch_resolution.py <SCCore.dll> fixtures/patch_resolution.json");
    }

    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    std::ifstream stream{fixture_path};
    REQUIRE(stream);
    const nlohmann::json document = nlohmann::json::parse(stream);

    std::size_t compared = 0;
    for (const auto& [map, map_name] : all_maps) {
        const auto& entry = document.at("maps").at(map_name);
        const auto& slots = entry.at("slots");
        INFO("map " << map_name);
        REQUIRE(slots.size() == entry.at("sounding").get<std::size_t>());

        std::size_t drum_space = 0;
        for (const auto& slot : slots) {
            const int bank = slot.at("bank").get<int>();
            const int program = slot.at("program").get<int>();

            const int tone_number = directory.program_to_tone(program, map, bank);
            if (tone_number != slot.at("tone").get<int>()) {
                FAIL("map " << map_name << " bank " << bank << " program " << program
                            << ": got tone " << tone_number << ", expected "
                            << slot.at("tone").get<int>());
            }

            // A handful of slots resolve into the drum tone space rather than the melodic table,
            // and the melodic table has no record for those -- the drum path picks them up instead.
            // The tone number is still compared above; only the name lookup is skipped.
            if (tone_number >= PatchDirectory::drum_space_start) {
                ++drum_space;
                ++compared;
                continue;
            }

            const std::optional<Tone> tone = directory.tone(tone_number);
            REQUIRE(tone.has_value());
            if (tone->name() != slot.at("name").get<std::string>()) {
                FAIL("map " << map_name << " bank " << bank << " program " << program
                            << ": got name '" << tone->name() << "', expected '"
                            << slot.at("name").get<std::string>() << "'");
            }
            ++compared;
        }

        // Only the SC-8820 map reaches the drum space this way, and only twice. Pinned because it
        // is the one case where a melodic program change lands somewhere the melodic table cannot
        // describe, and a resolver that quietly clamped it into range would still look correct.
        CHECK(drum_space == (map == ToneMap::sc8820 ? 2u : 0u));
    }

    // Guard against passing vacuously: an empty fixture would agree with anything.
    CHECK(compared == 4 * 128 * 128);
}

TEST_CASE("each vintage defines the banks its module had", "[patch][sccore]")
{
    // Independently pinned by the upstream C# suite at these same four numbers, and reproduced here
    // from the ROM rather than copied across. Each generation adds banks to the one before it, so a
    // lookup silently resolving against the wrong LUT1 row would show up as the wrong count.
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    const std::map<ToneMap, int> expected{
        {ToneMap::sc55, 15},
        {ToneMap::sc88, 24},
        {ToneMap::sc88pro, 45},
        {ToneMap::sc8820, 51},
    };

    std::vector<int> counts;
    for (const auto& [map, map_name] : all_maps) {
        std::set<int> banks_with_native;
        for (int bank = 0; bank < 128; ++bank) {
            for (int program = 0; program < 128; ++program) {
                const std::optional<int> raw = directory.lut3_raw(program, map, bank);
                if (raw && *raw != PatchDirectory::unassigned
                    && *raw < PatchDirectory::indirect_only_flag) {
                    banks_with_native.insert(bank);
                    break;
                }
            }
        }

        INFO("map " << map_name);
        CHECK(static_cast<int>(banks_with_native.size()) == expected.at(map));
        counts.push_back(static_cast<int>(banks_with_native.size()));
    }

    // Each generation is a superset of the one before it.
    for (std::size_t i = 1; i < counts.size(); ++i) {
        CHECK(counts[i] > counts[i - 1]);
    }
}

TEST_CASE("the lookup word is read unsigned", "[patch][sccore]")
{
    // Reading it signed and bailing on the sign bit is what once made four string patches look
    // unassigned: 0x8000 marks a tone reachable only through an alternate-articulation entry, not
    // a missing one. This asserts such words actually exist, so the distinction is exercised.
    const TableSet tables = load_tables();
    const auto lut3 = tables.dir_lut3();

    bool has_indirect = false;
    for (std::uint16_t word : lut3) {
        if (word >= PatchDirectory::indirect_only_flag && word != PatchDirectory::unassigned) {
            has_indirect = true;
            break;
        }
    }
    CHECK(has_indirect);
}

TEST_CASE("bank 0 is the capital bank and is fully populated", "[patch][sccore]")
{
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    for (int program = 0; program < 128; ++program) {
        INFO("program " << program);
        const int tone_number = directory.program_to_tone(program, ToneMap::sc8820, 0);
        REQUIRE(tone_number >= 0);
        const std::optional<Tone> tone = directory.tone(tone_number);
        REQUIRE(tone.has_value());
        CHECK(tone->is_defined());
    }
}

TEST_CASE("an empty variation slot falls back to the capital tone", "[patch][sccore]")
{
    // A Sound Canvas does not fall silent when a bank has no entry for a program; it sounds the
    // capital tone. Without this, one honky-tonk part in passport.mid selects bank 5 -- whose
    // program-3 slot is empty -- and every note is dropped.
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    std::size_t fallbacks = 0;
    for (int bank = 1; bank < 128; ++bank) {
        for (int program = 0; program < 128; ++program) {
            const std::optional<int> raw = directory.lut3_raw(program, ToneMap::sc8820, bank);
            if (raw && *raw != PatchDirectory::unassigned) {
                continue;
            }

            // This slot has no entry of its own, so it must resolve to whatever bank 0 sounds.
            const int fell_back = directory.program_to_tone(program, ToneMap::sc8820, bank);
            const int capital = directory.program_to_tone(program, ToneMap::sc8820, 0);
            INFO("bank " << bank << " program " << program);
            REQUIRE(fell_back == capital);
            ++fallbacks;
        }
    }

    // The fallback has to actually be exercised, or this proves nothing.
    CHECK(fallbacks > 0);
}

TEST_CASE("no tone in this ROM stores an inverted velocity window", "[patch][sccore]")
{
    // The upstream remark says the stored edges "may be the other way round", which reads as
    // something the data does. Measured across all 2363 defined tones and 3438 present partials in
    // this build: it never happens. The ordering in velocity_window() is therefore defensive rather
    // than load-bearing, and this test records that so the next person does not go looking for the
    // patch that exercises it.
    //
    // Pinned rather than deleted: if a future build does invert a window, that is worth knowing.
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    std::size_t defined = 0;
    std::size_t partials = 0;
    std::size_t inverted = 0;

    for (int tone_number = 0; tone_number < directory.tone_count(); ++tone_number) {
        const std::optional<Tone> tone = directory.tone(tone_number);
        if (!tone || !tone->is_defined()) {
            continue;
        }
        ++defined;
        for (const PartialParameters& partial : tone->partials()) {
            ++partials;
            if (partial.velocity_low() > partial.velocity_high()) {
                ++inverted;
            }
        }
    }

    CHECK(defined == 2363);
    CHECK(partials == 3438);
    CHECK(inverted == 0);
}

TEST_CASE("the velocity window orders its edges either way round", "[patch]")
{
    // Needs no DLL. Since the ROM never inverts a window, the ordering has to be exercised against
    // a synthetic block or it is untested code.
    std::array<std::uint8_t, PartialParameters::stride> block{};
    block[0x02] = 0x01; // a multisample, so the partial counts as present
    block[0x4F] = 100;  // velocity_low  edge, stored high
    block[0x51] = 20;   // velocity_high edge, stored low

    const PartialParameters partial{block.data()};
    REQUIRE(partial.is_present());

    const auto [low, high] = partial.velocity_window();
    CHECK(low == 20);
    CHECK(high == 100);

    CHECK(partial.accepts_velocity(20));
    CHECK(partial.accepts_velocity(60));
    CHECK(partial.accepts_velocity(100));
    CHECK_FALSE(partial.accepts_velocity(19));
    CHECK_FALSE(partial.accepts_velocity(101));
}

TEST_CASE("an empty bottom zone is silence, not the neighbouring wave", "[patch][sccore]")
{
    // Slap Bass 2 below note 40 is the case that proved it. The engine down-extrapolates
    // statically but terminates the voice, so falling through to the next wave up is audibly wrong.
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    std::size_t silent_bottoms = 0;
    for (int multisample = 0; multisample < directory.multisample_count(); ++multisample) {
        if (!directory.multisample_wave(multisample, 0).has_value()) {
            ++silent_bottoms;
        }
    }
    CHECK(silent_bottoms > 0);
}

TEST_CASE("half-damper is a piano feature", "[patch][sccore]")
{
    // Tone header byte 0x0d bit 2. Set on exactly 57 of the tones, matching the hardware, where
    // half-pedal is a piano feature and every other tone quantises the pedal to up or down.
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    std::size_t half_damper_tones = 0;
    for (int tone_number = 0; tone_number < directory.tone_count(); ++tone_number) {
        if (directory.half_damper(tone_number)) {
            ++half_damper_tones;
        }
    }

    CHECK(half_damper_tones == 57);
}

TEST_CASE("a played note resolves to the partials that sound", "[patch][sccore]")
{
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    // Acoustic grand, middle C, mezzo-forte.
    const ResolvedTone resolved = directory.resolve_midi(0, 60, 100, ToneMap::sc8820, 0);
    CHECK_FALSE(resolved.partials.empty());
    CHECK(resolved.name.size() >= 2);

    for (const ResolvedPartial& partial : resolved.partials) {
        INFO("partial " << partial.partial_index << " wave " << partial.wave);
        CHECK(partial.wave >= 0);
        CHECK(partial.wave < directory.wave_count());
        CHECK(partial.descriptor.start > partial.descriptor.loop);
    }

    // An unassigned program on an undefined map yields nothing rather than a stray tone.
    const ResolvedTone nothing = directory.resolve_midi(0, 60, 100, static_cast<ToneMap>(99), 0);
    CHECK(nothing.partials.empty());
}

TEST_CASE("out-of-range lookups return nothing rather than reading past a table", "[patch][sccore]")
{
    const TableSet tables = load_tables();
    const PatchDirectory directory{tables};

    CHECK_FALSE(directory.wave(-1).has_value());
    CHECK_FALSE(directory.wave(directory.wave_count()).has_value());
    CHECK_FALSE(directory.tone(-1).has_value());
    CHECK_FALSE(directory.tone(directory.tone_count()).has_value());
    CHECK_FALSE(directory.multisample_wave(-1, 60).has_value());
    CHECK_FALSE(directory.multisample_wave(directory.multisample_count(), 60).has_value());
    CHECK_FALSE(directory.alternate(-1).has_value());
    CHECK_FALSE(directory.alternate(directory.alternate_count()).has_value());
    CHECK(directory.zone_level(-1, 60, 0x40) == 127);
    CHECK_THROWS(directory.partial_by_slot(0, Tone::partial_slots));
}
