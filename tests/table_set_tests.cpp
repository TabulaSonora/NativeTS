#include "tabulasonora/table_set.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <stdexcept>

using namespace ts;
namespace fs = std::filesystem;

TEST_CASE("the table set loads identically from the DLL and from a cache", "[tables][sccore]")
{
    // Two independent load paths that must not diverge: one slices the DLL at manifest offsets, the
    // other reads the extracted .bin files. If they disagree, one of them is reading at the wrong
    // place and every table after it is silently shifted.
    const fs::path dll = testdata::require_sccore();
    const fs::path cache = testdata::require_tables();

    const RomImage rom = RomImage::open(dll.string(), RomVerification::quick);
    const TableSet from_rom = TableSet::from_rom(rom);
    const TableSet from_cache = TableSet::from_cache_directory(cache);

    std::size_t compared = 0;
    for (const TableEntry& entry : rom.manifest().cached_tables()) {
        INFO("table " << entry.name);
        REQUIRE(from_rom.raw(entry.name).size() == static_cast<std::size_t>(entry.size));
        const auto a = from_rom.raw(entry.name);
        const auto b = from_cache.raw(entry.name);
        REQUIRE(std::equal(a.begin(), a.end(), b.begin(), b.end()));
        ++compared;
    }
    CHECK(compared == 52);
}

TEST_CASE("element counts come from byte length, not the manifest shape", "[tables][sccore]")
{
    const fs::path cache = testdata::require_tables();
    const TableSet tables = TableSet::from_cache_directory(cache);

    // curve_level_2a00.bin is 256 bytes holding 128 uint16_t entries, yet its shape reads "256".
    // Deriving the count from the shape string would give twice as many entries as exist.
    CHECK(tables.raw(TableSet::names::level_curve).size() == 256);
    CHECK(tables.level_curve().size() == 128);

    // The amplitude curve is the pair the TVA multiplies together; both halves are 256 entries.
    CHECK(tables.amp_curve_hi().size() == 256);
    CHECK(tables.amp_curve_lo().size() == 256);

    // The rate-scale output curve is exactly 2^((i-0x80)/32) in 8.8, 512 entries.
    CHECK(tables.env_rate_out().size() == 512);
}

TEST_CASE("typed views reinterpret the bytes little-endian", "[tables][sccore]")
{
    const fs::path cache = testdata::require_tables();
    const TableSet tables = TableSet::from_cache_directory(cache);

    // Spot-check the widening against the raw bytes it came from. A byte-swapped or
    // wrong-stride reinterpretation produces plausible numbers, so this has to be checked
    // against the source bytes rather than against a range.
    const auto raw = tables.raw(TableSet::names::env_rate_out);
    const auto typed = tables.env_rate_out();
    REQUIRE(typed.size() * 2 == raw.size());

    for (std::size_t i = 0; i < typed.size(); ++i) {
        const auto expected = static_cast<std::uint16_t>(
            raw[i * 2] | (static_cast<std::uint16_t>(raw[i * 2 + 1]) << 8));
        REQUIRE(typed[i] == expected);
    }
}

TEST_CASE("the interpolator kernel is 128 phases of 4 taps summing to one", "[tables][sccore]")
{
    // This is the strongest single check that a float table survived the port: the 4-tap FIR
    // resample kernel is meaningless unless every phase sums to unity, and a misaligned or
    // byte-swapped read would not.
    const fs::path cache = testdata::require_tables();
    const TableSet tables = TableSet::from_cache_directory(cache);

    const auto coefficients = tables.interp_coef();
    REQUIRE(coefficients.size() >= 128 * 4);

    for (std::size_t phase = 0; phase < 128; ++phase) {
        const double sum = static_cast<double>(coefficients[phase * 4 + 0])
                           + static_cast<double>(coefficients[phase * 4 + 1])
                           + static_cast<double>(coefficients[phase * 4 + 2])
                           + static_cast<double>(coefficients[phase * 4 + 3]);
        // Measured: the row sums span 0.999999999970896 to 1.000009999999747, so the bound is
        // exactly 1e-5 either side and not some loose window. Row 0's fourth tap is literally 1e-5,
        // which is what sets the upper end.
        INFO("phase " << phase << " sums to " << sum);
        REQUIRE(std::abs(sum - 1.0) <= 1e-5);
    }
}

TEST_CASE("a cache that disagrees with the manifest is refused", "[tables]")
{
    // The sibling C# checkout's tables/ is exactly this case -- extracted against an older
    // manifest, so three files disagree. Reading it anyway would misalign real tables.
    const fs::path directory = fs::temp_directory_path() / "ts-bad-cache";
    fs::create_directories(directory);
    {
        std::ofstream stream{directory / std::string{TableSet::names::amp_curve_hi},
                             std::ios::binary};
        stream << "too short";
    }

    CHECK_THROWS_AS(TableSet::from_cache_directory(directory), std::runtime_error);
    fs::remove_all(directory);
}

TEST_CASE("an unknown table name is an error", "[tables][sccore]")
{
    const TableSet tables = TableSet::from_cache_directory(testdata::require_tables());
    CHECK_THROWS_AS(tables.raw("no_such_table.bin"), std::out_of_range);
}
