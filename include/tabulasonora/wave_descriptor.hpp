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
    /// `partial_compute_pitch @ 18005fc20` computes two native pitches —
    /// `voice+0x1fc = root*1000 - fine + 0x400`, then
    /// `voice+0x200 = voice+0x1fc - desc[0x0e] + 0x400` — and the exponent is taken against
    /// `voice+0x1fc`. That reads as though the second one goes nowhere, and every text search for
    /// `voice+0x200` agrees, which is how it was written off twice.
    ///
    /// It is read. `voices_control_update @ 1800849a0` walks the voices with its pointer at
    /// **`voice + 4`**, so the pair at `180084c13` —
    ///
    /// ```asm
    /// MOV EAX,dword ptr [RDI + 0x1fc]   ; voice+0x200
    /// MOV dword ptr [RDI + 0x1f8],EAX   ; voice+0x1fc
    /// LEA RCX,[RDI + -0x4]              ; the voice itself, for voice_block_process
    /// ```
    ///
    /// — is `voice+0x1fc = voice+0x200`. The four-byte skew is why no search for the literal offset
    /// ever found it.
    ///
    /// It is **not** run every tick, and it is **not** run at note-on. The condition, read off
    /// `voices_control_update` with its pointer at `voice + 4`:
    ///
    /// ```c
    /// if (voice+0x16c == 1) {                  // the ordinary sounding state
    ///     if (voice+4 != 0) {                  // a one-shot flag, cleared here
    ///         voice+4 = 0;
    ///         if (voice+0x1b0 != 0) { …retrigger…; voice+0x16c = 2; goto next; }
    ///         voice+0x1fc = voice+0x200;       // ← the term is adopted here, and only here
    ///     }
    ///     voice_block_process(voice);
    /// }
    /// ```
    ///
    /// So the term is adopted once, on a control tick where something else has already set
    /// `voice+4`, and never on the retrigger path. A fresh note-on does not set that flag, which is
    /// why `native_milli_semitones` leaves the term out.
    ///
    /// Confirmed on the engine rather than inferred: `scdec pitchword` reads both words off live
    /// voices, and for a struck note they stay *different* — Atmosphere's first partial sits at
    /// `0x1fc = 60243` against `0x200 = 60191`, and Synth Drum at `56316` against `56356`. Equal
    /// would mean adopted; they are not.
    ///
    /// An earlier note here recorded "84 of 254 non-neutral cases carry the term and 37 plainly do
    /// not" and concluded that applying it unconditionally was the closest available. That was
    /// measured against the module's pitch word across cases that include voices well past their
    /// note-on, which is exactly where the flag does get set — so it counted the adopted state
    /// without separating it from the struck one. Against rendered audio it is the wrong call: the
    /// 239-case oracle note gate goes from nine failing assertions to none by leaving it out.
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
    /// The module's `voice+0x1fc` *after* the control tick overwrites it with `voice+0x200` — both
    /// fine tunes, which is what a sounding note is tuned to. See `second_fine_tune`. Every ratio
    /// in this engine divides by this, so the sites must not drift apart.
    /// The native pitch a note-on plays at — `voice+0x1fc`, **without** `second_fine_tune`.
    ///
    /// The second term is not part of this. `partial_compute_pitch` writes it into a *separate*
    /// word, `voice+0x200`, and `voices_control_update` copies that over `voice+0x1fc` only under
    /// the condition recovered below — which a fresh note-on does not meet.
    [[nodiscard]] constexpr double native_milli_semitones() const noexcept
    {
        return (root_key * 1000.0) + 1024.0 - fine_tune;
    }

    /// The same, with `second_fine_tune` folded in — `voice+0x200`.
    ///
    /// Kept because the word is real and the engine does adopt it, just not when a note starts.
    [[nodiscard]] constexpr double adopted_milli_semitones() const noexcept
    {
        return native_milli_semitones() - (second_fine_tune - 1024.0);
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
