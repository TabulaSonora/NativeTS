#include "tabulasonora/build_registry.hpp"

#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/wave_rom.hpp"
#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace ts;
namespace fs = std::filesystem;

namespace {

/// Opens one of the alternate builds, skipping the case when that DLL is not present.
[[nodiscard]] RomImage require_build(const std::string& file_name)
{
    const auto path = testdata::alternate_build(file_name);
    if (!path) {
        SKIP("'" + file_name + "' not found. Place it at the repository root, or set the matching "
             "TS_SCCORE_* variable, to run the multi-build conformance cases.");
    }
    return RomImage::open(path->string(), RomVerification::full);
}

} // namespace

TEST_CASE("the registry describes every build the engine claims to read", "[rom][builds]")
{
    const BuildRegistry& registry = BuildRegistry::defaults();
    REQUIRE(registry.builds().size() >= 1);

    const auto pinned = std::count_if(registry.builds().begin(), registry.builds().end(),
                                      [](const BuildProfile& b) { return b.pinned(); });
    CHECK(pinned == 1);

    // The pinned build must be the one the manifest's offsets belong to, or every offset in the
    // engine is being translated from a coordinate system nothing recorded.
    CHECK(registry.pinned().identity().sha256 == TableManifest::defaults().dll().sha256);

    for (const BuildProfile& profile : registry.builds()) {
        CHECK(profile.identity().size > 0);
        CHECK(profile.identity().sha256.size() == 64);
        CHECK(profile.wave_rom_offset("wave_rom_bank_A").has_value());
        CHECK(profile.wave_rom_offset("wave_rom_bank_B").has_value());
    }
}

TEST_CASE("the pinned build maps to itself", "[rom][builds]")
{
    const BuildProfile& pinned = BuildRegistry::defaults().pinned();

    const auto pieces = pinned.map_range(0x19a1b88, 128);
    REQUIRE(pieces.size() == 1);
    CHECK(pieces.front().mapped());
    CHECK(pieces.front().target_offset == 0x19a1b88);
    CHECK(pieces.front().length == 128);
    CHECK(pinned.covers(0x19a1b88, 128));

    // Nothing in the pinned build can be uncoverable: the identity map has no holes.
    for (const TableEntry& entry : TableManifest::defaults().cached_tables()) {
        CHECK(pinned.covers(entry.file_offset, entry.size));
    }
}

TEST_CASE("segments are ordered, disjoint and non-empty", "[rom][builds]")
{
    for (const BuildProfile& profile : BuildRegistry::defaults().builds()) {
        const auto& segments = profile.segments();
        for (std::size_t i = 0; i < segments.size(); ++i) {
            CHECK(segments[i].length() > 0);
            if (i > 0) {
                CHECK(segments[i - 1].pinned_end <= segments[i].pinned_start);
            }
        }
    }
}

TEST_CASE("an alternate build is identified by content", "[rom][builds][sccore]")
{
    for (const char* name : {"SCCore.64.dll", "SCCore.32.dll"}) {
        const auto path = testdata::alternate_build(name);
        if (!path) {
            continue;
        }
        const RomImage rom = RomImage::open(path->string(), RomVerification::full);
        CHECK(rom.build().file_name() == name);
        CHECK_FALSE(rom.build().pinned());
        CHECK(rom.length() == rom.build().identity().size);
        CHECK(rom.compute_sha256() == rom.build().identity().sha256);
    }
}

TEST_CASE("a table that maps completely reads identically from every build", "[rom][builds][sccore]")
{
    const fs::path pinned_path = testdata::require_sccore();
    const RomImage pinned = RomImage::open(pinned_path.string(), RomVerification::full);

    for (const char* name : {"SCCore.64.dll", "SCCore.32.dll"}) {
        const auto path = testdata::alternate_build(name);
        if (!path) {
            continue;
        }
        const RomImage other = RomImage::open(path->string(), RomVerification::full);

        int compared = 0;
        for (const TableEntry& entry : TableManifest::defaults().cached_tables()) {
            if (!other.build().covers_table(entry)) {
                continue;
            }
            INFO("table " << entry.name << " from " << name);
            CHECK(other.read(entry) == pinned.read(entry));
            ++compared;
        }
        INFO(name);
        CHECK(compared > 0);
    }
}

TEST_CASE("an unmapped range is an error, never a partial read", "[rom][builds][sccore]")
{
    const RomImage rom = require_build("SCCore.64.dll");

    // Whatever this build cannot place must refuse to read rather than return zeroes for the hole:
    // silently wrong table data is far worse than a failed load.
    const auto incomplete = rom.build().incomplete_tables(TableManifest::defaults());
    if (incomplete.empty()) {
        SUCCEED("this build maps every table completely");
        return;
    }

    const TableEntry& entry = TableManifest::defaults().table(incomplete.front().name);
    CHECK_THROWS_AS(rom.read(entry), RomCoverageError);
}

TEST_CASE("the wave ROM is found per build, and holds the same samples", "[rom][builds][sccore]")
{
    const fs::path pinned_path = testdata::require_sccore();
    const RomImage pinned = RomImage::open(pinned_path.string(), RomVerification::full);

    for (const char* name : {"SCCore.64.dll", "SCCore.32.dll"}) {
        const auto path = testdata::alternate_build(name);
        if (!path) {
            continue;
        }
        const RomImage other = RomImage::open(path->string(), RomVerification::full);

        // Bank order differs between builds, so the offsets must differ while the bytes do not.
        INFO(name);
        CHECK(other.wave_rom_base("wave_rom_bank_A") != pinned.wave_rom_base("wave_rom_bank_A"));

        for (const char* bank : {"wave_rom_bank_A", "wave_rom_bank_B"}) {
            const std::int64_t a = pinned.wave_rom_base(bank);
            const std::int64_t b = other.wave_rom_base(bank);
            INFO(name << ' ' << bank);
            CHECK(other.read_raw(b, 4096) == pinned.read_raw(a, 4096));
            // A megabyte in, well past the block header, to catch a bank identified by its head only.
            CHECK(other.read_raw(b + 0x100000, 4096) == pinned.read_raw(a + 0x100000, 4096));
        }
    }
}

TEST_CASE("an unknown build is refused with a message naming what is known", "[rom][builds]")
{
    // A file that is the right shape but no build we know: the PE header of the pinned build with
    // one byte of it changed is indistinguishable from a stranger, which is the point.
    std::vector<std::uint8_t> bogus(4096, 0);
    bogus[0] = 'M';
    bogus[1] = 'Z';

    CHECK_THROWS_AS(RomImage::from_memory(bogus, RomVerification::full), RomIdentityError);

    try {
        [[maybe_unused]] const RomImage refused = RomImage::from_memory(bogus, RomVerification::full);
    } catch (const RomIdentityError& error) {
        const std::string text = error.what();
        CHECK(text.find("Known builds") != std::string::npos);
        CHECK(text.find("SCCore.dll") != std::string::npos);
    }
}
