#include "tabulasonora/table_manifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace ts;

// The manifest is the map every offset in the engine comes from, so these pin the identity of the
// build it is valid for. A different SCCore.dll moves every table, and the hash is what recognises
// that -- the DLL carries no version resource at all.

TEST_CASE("the embedded manifest pins the SOUND Canvas VA 1.1.6 build", "[manifest]")
{
    const TableManifest& manifest = TableManifest::defaults();
    const DllIdentity& dll = manifest.dll();

    CHECK(dll.file_name == "SCCore.dll");
    CHECK(dll.version == "1.1.6");
    CHECK(dll.size == 27'347'456);
    CHECK(dll.sha256 == "117e6aa147a96fbde5e10d2caf16c89965acc1e44235fd245992216cc620bdb1");
    CHECK(dll.pe_timestamp == 1'572'416'468u);
    CHECK(manifest.image_base() == 0x180000000);
}

TEST_CASE("every table in the manifest is addressable", "[manifest]")
{
    const TableManifest& manifest = TableManifest::defaults();

    REQUIRE(manifest.cached_tables().size() == 49);

    for (const TableEntry& entry : manifest.cached_tables()) {
        INFO("table " << entry.name);
        CHECK(entry.size > 0);
        CHECK(entry.file_offset > 0);
        CHECK(entry.file_offset + entry.size <= manifest.dll().size);
        CHECK(&manifest.table(entry.name) == &entry);
    }
}

TEST_CASE("hex offsets are parsed, not truncated", "[manifest]")
{
    const TableManifest& manifest = TableManifest::defaults();

    // The manifest records every offset as a hex string. These are past 2^24, so a parser that
    // silently stopped at the first non-digit or overflowed would be caught here.
    CHECK(manifest.table("curve_amp_hi_2ba0.bin").file_offset == 0x19a1ba0);
    CHECK(manifest.table("curve_amp_hi_2ba0.bin").size == 512);
    CHECK(manifest.table("tone_a.bin").file_offset == 0x18f1810);
    CHECK(manifest.table("tone_a.bin").size == 604'928);
}

TEST_CASE("the manifest is ahead of a stale extracted cache", "[manifest]")
{
    // Recorded because it bit during the port: the sibling C# checkout's tables/ directory was
    // extracted against an older manifest and disagrees with this one in three places -- it has no
    // porta_step_7800.bin at all, and tone_a.bin and wavedesc_a.bin have both since grown.
    //
    // A cache-directory loader validates each file's length against these sizes, so loading that
    // stale directory is an error rather than a silent short read. These are the current sizes; if
    // this fails, the manifest moved and the caches need re-extracting from the DLL.
    const TableManifest& manifest = TableManifest::defaults();

    CHECK(manifest.table("porta_step_7800.bin").size == 256);
    CHECK(manifest.table("tone_a.bin").size == 604'928);
    CHECK(manifest.table("wavedesc_a.bin").size == 93'698);
}

TEST_CASE("live regions address the file uniformly", "[manifest]")
{
    const TableManifest& manifest = TableManifest::defaults();

    REQUIRE(manifest.live_regions().size() == 5);

    // Wave-ROM banks carry 'file_offset'; the drum tables carry 'va', but those values are already
    // file offsets. Both must surface as a file offset that actually lands inside the DLL.
    const LiveRegion& bank_a = manifest.region("wave_rom_bank_A");
    CHECK(bank_a.file_offset == 0x92700);
    REQUIRE(bank_a.size.has_value());
    CHECK(*bank_a.size == 12'582'912);

    const LiveRegion& drums = manifest.region("drum_prog_map");
    CHECK(drums.file_offset > 0);
    CHECK(drums.file_offset < manifest.dll().size);
}

TEST_CASE("an unknown name is an error, not a default", "[manifest]")
{
    const TableManifest& manifest = TableManifest::defaults();

    CHECK_THROWS_AS(manifest.table("no_such_table.bin"), std::out_of_range);
    CHECK_THROWS_AS(manifest.region("no_such_region"), std::out_of_range);
}

TEST_CASE("a malformed manifest is rejected", "[manifest]")
{
    CHECK_THROWS(TableManifest::parse("{}"));
    CHECK_THROWS(TableManifest::parse("not json at all"));
}
