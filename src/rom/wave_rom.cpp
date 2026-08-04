#include "tabulasonora/wave_rom.hpp"

namespace ts {

WaveRom::WaveRom(const RomImage& rom)
    : rom_(&rom),
      bank_a_base_(rom.manifest().region("wave_rom_bank_A").file_offset),
      bank_b_base_(rom.manifest().region("wave_rom_bank_B").file_offset)
{
}

std::int64_t WaveRom::region_base(int region) const noexcept
{
    const SplitRegion split = split_region(region);
    return bank_base(split.bank)
           + (static_cast<std::int64_t>(split.effective_region) * region_size);
}

std::optional<WaveStreams> WaveRom::read_streams(int region, int loop, int start) const
{
    const std::int32_t sample_count = start - loop;
    if (sample_count <= 0 || sample_count > max_sample_count) {
        return std::nullopt;
    }

    const std::int64_t base = region_base(region);
    const std::int32_t scale_phase = loop & 0x1F;
    const std::int64_t delta_offset = base + loop;
    const std::int64_t scale_offset = base + (loop >> 5);

    // One extra delta: the ping-pong sampler applies the step at the turnaround index.
    const std::int32_t delta_length = sample_count + 1;
    const std::int32_t scale_length = ((scale_phase + delta_length) >> 5) + 4;

    WaveStreams streams;
    streams.delta = rom_->read(delta_offset, static_cast<std::size_t>(delta_length));
    streams.scale = rom_->read(scale_offset, static_cast<std::size_t>(scale_length));
    if (scale_phase > 0) {
        // The block-boundary preamble, which seeds the decode -- see the field's own comment.
        // Its exponents are the same scale byte the phase indexes into, already read above.
        streams.preamble_delta =
            rom_->read(base + (loop & ~0x1F), static_cast<std::size_t>(scale_phase));
    }
    streams.sample_count = sample_count;
    streams.data_start = loop;
    streams.scale_phase = scale_phase;
    return streams;
}

} // namespace ts
