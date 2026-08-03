#pragma once

#include "tabulasonora/rom_image.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace ts {

/// The two raw streams that make up one wave: a signed delta per sample, and a shift exponent per
/// 16-sample block packed two to a byte.
struct WaveStreams {
    /// One signed byte per sample, `sample_count + 1` long — the ping-pong sampler turns around
    /// *on* index `sample_count`, so that extra step is read.
    std::vector<std::uint8_t> delta;
    /// Shift-exponent nibbles: byte `i >> 5` holds the exponents for samples `i`, with the low
    /// nibble used when `(i >> 4) & 1` is zero and the high nibble otherwise.
    std::vector<std::uint8_t> scale;
    /// Number of decodable samples.
    std::int32_t sample_count = 0;
    /// The descriptor's loop start rounded down to a 32-sample boundary.
    std::int32_t aligned_loop = 0;
};

/// The 24 MB wave ROM embedded in `SCCore.dll` — the literal Sound Canvas hardware mask ROM, two
/// banks addressed in 1 MB regions.
///
/// Bank A is the stacked SC-88/SC-88Pro ROM, bank B the SC-8820 ROM. Within a region the shift
/// exponents live at the bottom (one byte per 32 samples, so 32 KB covers the whole 1 MB) while the
/// deltas are addressed directly by sample index.
class WaveRom {
public:
    /// Size of one addressable ROM region, 1 MB.
    static constexpr std::int32_t region_size = 0x100000;

    /// Largest wave this class will attempt to read, as a sanity bound.
    static constexpr std::int32_t max_sample_count = 2'000'000;

    /// Regions of real ROM data in bank A.
    ///
    /// **`region_base` does not enforce this, and descriptors exist that exceed it.** Wave 4010
    /// (TR-808 open hat) declares bank 0 region 12 and wave 2092 (Bim Hit) region 14, both past the
    /// twelve regions this bound describes, so their reads run off the end of the declared bank.
    /// Both decode measurably wrong; every wave measured inside the bound decodes exactly. Compared
    /// against the reference's own filter-input buffer, per 32-sample window: waves 2818 (region 5)
    /// and 4008 (bank 1) match at r = 1.00000, wave 4010 at 0.88, wave 2092 at 0.81 and falling.
    /// Wave 4010's error is a constant +0.28-sample offset, which is what makes the open hat render
    /// about 1.1 dB loud and bright. Unreversed: what the engine's own region map does past 11.
    static constexpr int bank_a_region_count = 12;

    /// Regions of real ROM data in bank B — eight, not twelve.
    ///
    /// The manifest records both banks as 12 MB, but bank B's data ends at file offset `0x1892730`:
    /// regions 0–3 are the tail of the 1996 `rom_make` image and regions 4–7 are the 1999
    /// `8820_wv0` image. The declared 12 MB span runs past the end of the file altogether, so the
    /// nominal size cannot be used as a bound.
    static constexpr int bank_b_region_count = 8;

    /// Number of regions of real ROM data in a bank: 0 for bank A, 1 for bank B.
    [[nodiscard]] static constexpr int region_count(int bank) noexcept
    {
        return bank == 0 ? bank_a_region_count : bank_b_region_count;
    }

    /// Splits a descriptor's region byte into a bank and a bank-relative region index.
    struct SplitRegion {
        int bank = 0;
        int effective_region = 0;
    };

    [[nodiscard]] static constexpr SplitRegion split_region(int region) noexcept
    {
        const int bank = (region >> 4) & 1;
        return SplitRegion{bank, region - (16 * bank)};
    }

    /// Creates a wave-ROM view over an open image, which must outlive it.
    explicit WaveRom(const RomImage& rom);

    /// File offset of the first byte of a bank.
    [[nodiscard]] std::int64_t bank_base(int bank) const noexcept
    {
        return bank == 0 ? bank_a_base_ : bank_b_base_;
    }

    /// File offset of the base of a region, from a descriptor's region byte.
    [[nodiscard]] std::int64_t region_base(int region) const noexcept;

    /// Reads the delta and scale streams for one wave.
    ///
    /// Returns nothing when the descriptor describes no usable data — a non-positive or implausibly
    /// large length.
    ///
    /// The descriptor's field names are as recovered, and they are confusing: `loop` is the data
    /// start, `end` is the loop point, and `start` is the physical end.
    ///
    /// **`aligned_loop = loop & ~0x1F` is suspect for the waves it actually truncates.** Nearly
    /// every `loop` is already a multiple of 32, but wave 2092's is 896066, and rounding it down by
    /// two shifts every shift-exponent block against the deltas it scales. Since the decoder is a
    /// pure delta accumulator with no DC blocker anywhere downstream, one wrongly-scaled block
    /// displaces the running predictor for the rest of the wave rather than colouring 32 samples —
    /// which matches the progressive divergence measured on that wave, as distinct from the constant
    /// offset seen on wave 4010. Not yet confirmed against the reference's own decode.
    [[nodiscard]] std::optional<WaveStreams> read_streams(int region, int loop, int start) const;

private:
    const RomImage* rom_;
    std::int64_t bank_a_base_ = 0;
    std::int64_t bank_b_base_ = 0;
};

} // namespace ts
