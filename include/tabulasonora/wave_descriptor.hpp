#pragma once

#include <cstdint>
#include <span>

namespace ts {

/// One entry of the wave-descriptor table: where a sample lives in the wave ROM, how it is tuned,
/// and how it loops.
///
/// The field names are as recovered and they are genuinely confusing: `loop` is the data start,
/// `end` is the loop point, and `start` is the physical end. They are kept rather than renamed so
/// the code lines up with the reverse-engineering notes and the trace dumps.
struct WaveDescriptor {
    /// Bytes per descriptor record.
    static constexpr int stride = 0x16;

    /// ROM region, 0–127. Bit 4 selects the bank.
    int region = 0;
    /// Start of the sample data — delta index zero.
    int loop = 0;
    /// The loop point.
    int end = 0;
    /// The physical end of the data.
    int start = 0;
    /// MIDI note at which the sample plays back untransposed.
    int root_key = 0;
    /// Fine-tune word; native pitch is `root_key + (1024 - fine_tune) / 1000`.
    int fine_tune = 0;
    /// Second fine-tune word, at descriptor `0x0E`, neutral at 1024 like the first.
    ///
    /// **Real data the module does not tune with.** `partial_compute_pitch @ 18005fc20` computes
    /// two native pitches — `voice+0x1fc = root*1000 - fine + 0x400` and
    /// `voice+0x200 = voice+0x1fc - desc[0x0e] + 0x400` — and only the first is ever read: both
    /// `voice_pitch_keyfollow` and `voice_pitch_block_update` subtract `voice+0x1fc` to form the
    /// exponent, and `voice+0x200` is written once and read nowhere in the binary.
    ///
    /// The descriptor those two are computed from is not the ROM record directly but a staging
    /// copy at `0x181a1e81e`, which `LAB_18005c390` returns and `partial_load_params` fills with a
    /// verbatim 22-byte copy of the record — so the fields are the record's, unmodified, and this
    /// one really does go nowhere. Kept because it is decoded, and because an engine that stopped
    /// reading it would lose the ability to say so.
    int second_fine_tune = 1024;
    /// Raw flag byte. Bit 0 is bidirectional, bit 2 is reverse; bit 1 takes no part in the
    /// dispatch.
    int flags = 0;

    /// ROM bank: 0 for bank A, 1 for bank B.
    [[nodiscard]] constexpr int bank() const noexcept { return (region >> 4) & 1; }

    /// True when the sampler plays this wave backwards.
    [[nodiscard]] constexpr bool reverse() const noexcept { return ((flags >> 2) & 1) != 0; }

    /// True when the sampler plays this wave bidirectionally (ping-pong).
    [[nodiscard]] constexpr bool ping_pong() const noexcept { return (flags & 1) != 0; }

    /// Native pitch in semitones — the effective root, including both fine tunes.
    [[nodiscard]] constexpr double native_pitch() const noexcept
    {
        return native_milli_semitones() / 1000.0;
    }

    /// Native pitch in milli-semitones — what the sampler's ratio is taken against.
    ///
    /// The module's `voice+0x1fc`, and only that: the root and the *first* fine tune. See
    /// `second_fine_tune` for why the other one is not here. Every ratio in this engine divides by
    /// this, so the sites must not drift apart.
    [[nodiscard]] constexpr double native_milli_semitones() const noexcept
    {
        return (root_key * 1000.0) + 1024.0 - fine_tune;
    }

    /// Parses a descriptor from its `stride` raw bytes.
    [[nodiscard]] static constexpr WaveDescriptor
    parse(std::span<const std::uint8_t> record) noexcept
    {
        // The three position fields are 20-bit, stored most-significant nibble first.
        const int loop = ((record[0x01] & 0x0F) << 16) | (record[0x02] << 8) | record[0x03];
        const int end = ((record[0x07] & 0x0F) << 16) | (record[0x08] << 8) | record[0x09];
        const int start = ((record[0x0B] & 0x0F) << 16) | (record[0x0C] << 8) | record[0x0D];

        return WaveDescriptor{
            .region = record[0x00] & 0x7F,
            .loop = loop,
            .end = end,
            .start = start,
            .root_key = record[0x06],
            .fine_tune = record[0x04] | (record[0x05] << 8),
            .second_fine_tune = record[0x0E] | (record[0x0F] << 8),
            .flags = record[0x0A],
        };
    }

    [[nodiscard]] friend constexpr bool operator==(const WaveDescriptor&,
                                                   const WaveDescriptor&) noexcept = default;
};

/// A key range within a multisample and the wave it selects.
struct MultisampleZone {
    int key_low = 0;
    int key_high = 0;
    int wave = 0;
};

} // namespace ts
