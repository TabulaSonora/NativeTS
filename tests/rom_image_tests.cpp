#include "tabulasonora/rom_image.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

using namespace ts;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] std::vector<std::uint8_t> read_whole_file(const fs::path& path)
{
    std::ifstream stream{path, std::ios::binary};
    REQUIRE(stream);
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>{stream},
                                     std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("the pinned DLL verifies end to end", "[rom][sccore]")
{
    const fs::path path = testdata::require_sccore();
    const RomImage rom = RomImage::open(path.string(), RomVerification::full);

    // Full verification already checked all three; asserting them again states what was checked.
    CHECK(rom.length() == rom.manifest().dll().size);
    CHECK(rom.read_pe_timestamp() == rom.manifest().dll().pe_timestamp);
    CHECK(rom.compute_sha256() == rom.manifest().dll().sha256);
}

TEST_CASE("every table reads byte-identical to the extracted cache", "[rom][sccore][gate]")
{
    // The Phase 1 gate. Reading a table is nothing but a slice at a manifest offset, so if these
    // 49 comparisons hold then the offset map, the hex parsing and the positional reads are all
    // correct together -- and every later phase can trust the bytes it is handed.
    const fs::path dll = testdata::require_sccore();
    const fs::path cache = testdata::require_tables();

    const RomImage rom = RomImage::open(dll.string(), RomVerification::quick);

    std::size_t compared = 0;
    std::int64_t bytes = 0;

    for (const TableEntry& entry : rom.manifest().cached_tables()) {
        const fs::path expected_path = cache / entry.name;
        if (!fs::exists(expected_path)) {
            FAIL("Table cache '" << entry.name << "' is missing from " << cache
                                 << ". Re-run extract-tables; the manifest may be ahead of it.");
        }

        INFO("table " << entry.name << " at 0x" << std::hex << entry.file_offset);

        const std::vector<std::uint8_t> expected = read_whole_file(expected_path);
        REQUIRE(expected.size() == static_cast<std::size_t>(entry.size));

        const std::vector<std::uint8_t> actual = rom.read(entry);
        REQUIRE(actual.size() == expected.size());
        REQUIRE(actual == expected);

        ++compared;
        bytes += entry.size;
    }

    // Guard against passing vacuously: a loop over an empty manifest would agree perfectly.
    CHECK(compared == 52);
    CHECK(bytes == 1'218'200);
}

TEST_CASE("a memory image reads the same as a file image", "[rom][sccore]")
{
    // The browser and any host without a filesystem take this path, and it must not diverge --
    // nothing is copied, the bytes are read in place.
    const fs::path path = testdata::require_sccore();

    const std::vector<std::uint8_t> bytes = read_whole_file(path);
    const RomImage from_memory = RomImage::from_memory(bytes, RomVerification::full);
    const RomImage from_file = RomImage::open(path.string(), RomVerification::quick);

    CHECK(from_memory.length() == from_file.length());
    CHECK(from_memory.path() == "<memory>");

    for (const TableEntry& entry : from_file.manifest().cached_tables()) {
        INFO("table " << entry.name);
        REQUIRE(from_memory.read(entry) == from_file.read(entry));
    }
}

TEST_CASE("a file that is not the pinned build is refused", "[rom]")
{
    // Needs no DLL: any file of the wrong size fails on the very first check, and that check is
    // what stops a different SCCore.dll from being read at offsets that no longer mean anything.
    const fs::path temporary = fs::temp_directory_path() / "ts-not-sccore.bin";
    {
        std::ofstream stream{temporary, std::ios::binary};
        stream << "not a Sound Canvas ROM";
    }

    CHECK_THROWS_AS(RomImage::open(temporary.string(), RomVerification::full), RomIdentityError);
    CHECK_THROWS_AS(RomImage::open(temporary.string(), RomVerification::quick), RomIdentityError);

    // ...but 'none' is an explicit opt-out for experimenting with another build.
    CHECK_NOTHROW(RomImage::open(temporary.string(), RomVerification::none));

    fs::remove(temporary);
}

TEST_CASE("a missing file is an error, not an empty image", "[rom]")
{
    CHECK_THROWS(RomImage::open("/no/such/SCCore.dll", RomVerification::none));
}
