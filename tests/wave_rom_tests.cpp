#include "tabulasonora/wave_rom.hpp"

#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ts;
namespace fs = std::filesystem;

TEST_CASE("region bytes split into a bank and an index", "[waverom]")
{
    // Needs no DLL. Bit 4 of the descriptor's region byte selects the bank, and the index within
    // the bank is what is left after removing it.
    CHECK(WaveRom::split_region(0).bank == 0);
    CHECK(WaveRom::split_region(0).effective_region == 0);

    CHECK(WaveRom::split_region(11).bank == 0);
    CHECK(WaveRom::split_region(11).effective_region == 11);

    CHECK(WaveRom::split_region(16).bank == 1);
    CHECK(WaveRom::split_region(16).effective_region == 0);

    CHECK(WaveRom::split_region(23).bank == 1);
    CHECK(WaveRom::split_region(23).effective_region == 7);
}

TEST_CASE("bank A spans sixteen regions and bank B only eight", "[waverom]")
{
    // The manifest declares both banks as 12 MB. Bank A is wider than that -- the two bank bases
    // are 16 MB apart, so every region a descriptor's four-bit field can name is real, and
    // descriptors do reach region 14. Bank B is the one that stops early: its data ends before the
    // declared span, so trusting the nominal size there reads garbage, or nothing.
    CHECK(WaveRom::region_count(0) == 16);
    CHECK(WaveRom::region_count(1) == 8);
}

TEST_CASE("bank A is wide enough for every region a descriptor can name", "[waverom][sccore]")
{
    const fs::path path = testdata::require_sccore();
    const RomImage rom = RomImage::open(path.string(), RomVerification::quick);
    const WaveRom waves{rom};

    // Regions 12 and 14 are used (waves 4010 and 2092). They must land inside bank A, not spill
    // into bank B -- which is what makes `bank_a_base + region * 1 MB` correct for all sixteen.
    for (int region = 0; region < WaveRom::region_count(0); ++region) {
        const std::int64_t base = waves.region_base(region);
        CHECK(base >= waves.bank_base(0));
        CHECK(base + WaveRom::region_size <= waves.bank_base(1));
    }
}

TEST_CASE("every real region lies inside the file", "[waverom][sccore]")
{
    const fs::path path = testdata::require_sccore();
    const RomImage rom = RomImage::open(path.string(), RomVerification::quick);
    const WaveRom waves{rom};

    CHECK(waves.bank_base(0) == 0x92700);
    CHECK(waves.bank_base(1) == 0x1092730);

    for (int bank = 0; bank < 2; ++bank) {
        for (int region = 0; region < WaveRom::region_count(bank); ++region) {
            const int descriptor_byte = region + (16 * bank);
            const std::int64_t base = waves.region_base(descriptor_byte);
            INFO("bank " << bank << " region " << region);
            REQUIRE(base >= 0);
            REQUIRE(base + WaveRom::region_size <= rom.length());
        }
    }

    // Bank B's wave data ends exactly where its ninth region would start. Regions 0-3 are the tail
    // of the 1996 rom_make image and 4-7 are the 1999 8820_wv0 image; past that is not wave ROM.
    CHECK(waves.region_base(16 + WaveRom::bank_b_region_count) == 0x1892730);

    // And the manifest's declared 12 MB for that bank runs past the end of the file altogether,
    // which is why the nominal size cannot be used as a bound and the count is hard-coded at eight.
    const LiveRegion& declared = rom.manifest().region("wave_rom_bank_B");
    REQUIRE(declared.size.has_value());
    CHECK(declared.file_offset + *declared.size > rom.length());
}

TEST_CASE("a wave reads its delta and scale streams", "[waverom][sccore]")
{
    const fs::path path = testdata::require_sccore();
    const RomImage rom = RomImage::open(path.string(), RomVerification::quick);
    const WaveRom waves{rom};

    // loop is the data start, start is the physical end -- the descriptor's field names are as
    // recovered and are confusing. A 4096-sample span from an aligned start.
    const auto streams = waves.read_streams(0, 0x1000, 0x2000);
    REQUIRE(streams.has_value());

    CHECK(streams->data_start == 0x1000);
    CHECK(streams->scale_phase == 0);
    CHECK(streams->sample_count == 0x1000);

    // One extra delta: the ping-pong sampler applies the step at the turnaround index.
    CHECK(streams->delta.size() == 0x1001);

    // One scale byte covers 32 samples, plus a small over-read so the last block is complete.
    CHECK(streams->scale.size() == static_cast<std::size_t>((0x1001 >> 5) + 4));
}

TEST_CASE("an unaligned data start is kept, not rounded to a block", "[waverom][sccore]")
{
    const fs::path path = testdata::require_sccore();
    const RomImage rom = RomImage::open(path.string(), RomVerification::quick);
    const WaveRom waves{rom};

    // The codec stores no absolute value per block, only differences, so a wave may begin partway
    // into an exponent block. Decoding carries the phase and indexes the exponents absolutely
    // rather than rounding the start down, which would begin integrating early and -- with no leak
    // in the predictor and no DC blocker downstream -- displace the whole wave.
    const auto streams = waves.read_streams(0, 0x101F, 0x2000);
    REQUIRE(streams.has_value());
    CHECK(streams->data_start == 0x101F);
    CHECK(streams->scale_phase == 0x1F);
    CHECK(streams->sample_count == 0x2000 - 0x101F);

    // The scale stream has to cover the phase as well as the samples.
    CHECK(streams->scale.size()
          == static_cast<std::size_t>(((0x1F + (0x2000 - 0x101F) + 1) >> 5) + 4));

    // The last sample's exponent must be readable: absolute position phase + count - 1.
    const int last = streams->scale_phase + streams->sample_count;
    CHECK(static_cast<std::size_t>(last >> 5) < streams->scale.size());
}

TEST_CASE("a descriptor with no usable data yields nothing", "[waverom][sccore]")
{
    const fs::path path = testdata::require_sccore();
    const RomImage rom = RomImage::open(path.string(), RomVerification::quick);
    const WaveRom waves{rom};

    // Returning empty streams instead would make a silent voice look like a working one.
    CHECK_FALSE(waves.read_streams(0, 0x2000, 0x2000).has_value());
    CHECK_FALSE(waves.read_streams(0, 0x2000, 0x1000).has_value());
    CHECK_FALSE(waves.read_streams(0, 0, WaveRom::max_sample_count + 1).has_value());
}
