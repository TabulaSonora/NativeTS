#pragma once

#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/rom_image.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ts {

/// One drum key's settings within a kit.
struct DrumKey {
    /// Tone number. Drum sounds are ordinary melodic tones.
    int tone = 0;
    /// Kit level for this key. Applies downstream as `(level / 127)^2`.
    int level = 0;
    /// Coarse pitch, 60 being the tone's natural pitch.
    int pitch = 0;
    /// Mute/assign group; keys sharing a non-zero group cut each other off.
    int group = 0;
    /// Pan position, per key.
    int pan = 0;
    /// Reverb send depth for this key, 0-127, scaling the part's own send rather than replacing it.
    ///
    /// This is per *key*, not per part, and the difference is audible: in `STANDARD 1` the kick
    /// (36) reads 0 while the snare (38) and crash (49) read 127, so a kit with the part's send
    /// wide open still has a dry kick. Applying the part's send uniformly puts a room on the one
    /// drum the module deliberately keeps out of it.
    ///
    /// Measured against the module, which multiplies the two: driving key 36 with the send at
    /// CC91 64 and 127 against per-key 64 and 127 gives reverb tails 0.0012, 0.0024, 0.0024 and
    /// 0.0048 — doubling either input doubles the tail, and a zero on either side is silence.
    int reverb = 0;
    /// Chorus send depth for this key, on the same law as `reverb`.
    ///
    /// Measured the same way and it behaves the same: per-key against CC#93, the off-diagonal is
    /// symmetric to the last digit (64x127 and 127x64 both 0.0441678 over the hit) and taking the
    /// wet out by power subtraction gives 0.01603, 0.02947, 0.05575 -- doubling either input
    /// doubles the wet amplitude. It has to be measured over the hit rather than after it: a
    /// chorus is a twenty to thirty millisecond modulated delay and leaves no tail where reverb
    /// and delay leave one.
    int chorus = 0;
    /// Delay send depth for this key, on the same law again.
    ///
    /// Per-key against CC#94, tail 0.5-1.5 s after the hit: 0.0023021, 0.0046040, 0.0046040,
    /// 0.0091360 -- a product, silent whenever either side is zero.
    int delay = 0;
    /// Whether this key responds to note-off at all — GS `Rx.Note Off`, per key.
    ///
    /// Almost none do. A drum is a struck sound whose envelope is its whole story, so releasing it
    /// early is wrong; the exceptions are the sounds you have to be able to stop, and the kit
    /// records say exactly which. In the SC-88 Standard kit **one** key sets it, key 25, the snare
    /// roll. The Orchestra kit adds key 88, Applause. The SFX kit sets it on 52 of its 128 keys.
    /// That is GS's documented behaviour read straight out of the ROM rather than assumed.
    ///
    /// Bit 0 of the kit record's receive plane. The engine copies that plane per part and lets
    /// drum-setup SysEx rewrite it, so this is the *default*, not the last word.
    bool receives_note_off = false;

    /// Whether this key responds to note-on — GS `Rx.Note On`, bit 4 of the same plane.
    ///
    /// The module's note-on dispatch refuses the key outright when this is off
    /// (`note_on_dispatch` tests `0x480[key] & 0x10` before anything else), which is how a file
    /// silences individual drum keys without emptying their track.
    bool receives_note_on = true;
};

/// The drum kit records: for each of 128 keys, which tone sounds and how it is levelled, tuned,
/// panned and grouped.
///
/// Three things about drums are easy to get wrong, and all three are settled by measurement:
///
///  * Drum sounds are ordinary *melodic* tones. They do not use the separate drum tone table
///    (stride 0x1e8), which is why that table's remaining unreversed state does not block GM kits.
///  * They do not resolve through the three-level melodic lookup; a program change on the drum part
///    selects a kit through its own pair of lookups.
///  * The note does not transpose the sample. The kit's coarse-pitch plane supplies the key
///    instead, pivoting on 60, and the tone's own key-follow decides what a step of it is worth.
class DrumKitTable {
public:
    /// Bytes per kit record.
    static constexpr int kit_stride = 0x50C;

    /// Keys per kit.
    static constexpr int key_count = 128;

    /// Rows in the drum program map.
    ///
    /// Six, not the two this read for a long time. Each row maps a program to a kit, and program 0
    /// resolves to kit 0, 38, 47, 59, 68 and 79 across rows 0 to 5. Reading two rows put every kit
    /// from 47 up out of reach entirely.
    ///
    /// Measured against the DLL: driven with the SC-55 tone map on program 0 of the drum part, the
    /// engine resolves a tone for all 61 sounding keys that *kit 59* reproduces exactly, 61 of 61 —
    /// and kit 59 is only reachable through row 3. The best any kit the two-row read could see
    /// managed was 30 of 61.
    static constexpr int map_row_count = 6;

    /// File offset of the table mapping a level-two index to a kit record index.
    ///
    /// Unlike the other three drum regions this one is not recorded in the manifest, so it is
    /// pinned here. It is `DAT_1819f31b0` in the decompile.
    static constexpr std::int64_t kit_index_offset = 0x19F21B0;

    /// Creates the table by reading the drum regions out of the ROM image.
    explicit DrumKitTable(const RomImage& rom);

    /// Number of kit records reachable through the program map.
    [[nodiscard]] int kit_count() const noexcept { return kit_count_; }

    /// The drum map row a vintage's tone map selects, or nothing where no measurement pins one.
    ///
    /// The module takes this from the part's internal bank code, and that translation is not
    /// reversed — but which row each vintage ends up on is observable. Driving the DLL with each
    /// tone map in turn and reading back the tone it resolves for program 0 on the drum part:
    /// SC-55 selects row 3 (kit 59), SC-88 row 2 (kit 47), SC-88Pro row 1 and SC-8820 row 0.
    ///
    /// Rows 4 and 5 are not vintages: row 4 is the XG kit set — Standard 1/2, Room, Rock, Electro,
    /// Analog, Jazz, Brush, Classic on programs 0/1/8/16/24/25/32/40/48, plus SFX 1 and 2 on 120
    /// and 121 — and row 5 is GM2's. `ToneMap::xg` selects row 4; a host that wants row 5 sets it
    /// itself.
    [[nodiscard]] static std::optional<int> row_for_map(ToneMap map) noexcept;

    /// Maps an internal bank code to a drum map row; standard GM/GS drum parts use 0x04.
    [[nodiscard]] int map_row(int internal_bank) const noexcept;

    /// Resolves a drum program to its kit record index, or nothing for an undefined program.
    ///
    /// The engine leaves the kit unchanged rather than silencing the part, which is why this is an
    /// absence rather than a zero.
    [[nodiscard]] std::optional<int> kit_for_program(int program, int row = 0) const noexcept;

    /// The kit's name, as the ROM record carries it, trimmed of trailing spaces.
    ///
    /// Twelve bytes at `+0x500` of the record, the same field the module's rhythm-set name dump
    /// reads. A mixer that shows "kit 73" is showing an index nobody chose; the record has been
    /// carrying "Analog Kit" all along.
    ///
    /// Three things not to assume about the result, all measured and written up in FINDINGS under
    /// "Kit names, and three things not to assume about them":
    ///
    ///  * **The casing is the ROM's and is not normalised here.** Every GS and GM2 kit is upper
    ///    case and every XG kit is lower, which makes an ALL-CAPS name on XG-flavoured material a
    ///    free signal that the drum row is not following XG mode.
    ///  * **A name is not an identifier.** Fifteen are shared by two to four records — `ROOM` is
    ///    four, one per GS vintage — so anything keyed on the name collides across exactly the
    ///    vintages a tone map exists to separate.
    ///  * **A twelve-byte name may be abbreviated**, and not always at the end: `standrd kit2`
    ///    drops the *a* of "standard", `GM2 ORCHSTRA` the *E* of "ORCHESTRA".
    ///
    /// Empty for a kit index outside the table.
    [[nodiscard]] std::string kit_name(int kit) const;

    /// Reads one key's settings from a kit; kit 0 is GM Standard.
    ///
    /// Throws `std::out_of_range` if the note is outside 0–127.
    [[nodiscard]] DrumKey key(int note, int kit = 0) const;

    /// The playback ratio the kit's coarse-pitch plane implies.
    ///
    /// **Only correct for a tone that key-follows at 50%**, which is what the SC-88 and SC-8820
    /// standard kits use and what this constant was measured from. The general rule is per-tone:
    /// the SC-55 kits and the Electronic family follow at 100% and move a whole semitone per unit,
    /// so they come out half as far under this reading. The engine paths use
    /// `PitchChain::drum_pitch_milli_semitones`; this remains for callers that want the old scalar.
    [[nodiscard]] static double coarse_pitch_ratio(int pitch) noexcept;

    /// The gain the kit level contributes, as amplitude.
    ///
    /// The kit level is *not* part of the per-voice amplitude — that matches the engine exactly
    /// without it. It enters downstream in the part-volume computation, where the result is
    /// squared, so it acts as `(level / 127)^2`.
    [[nodiscard]] static constexpr double level_gain(int level) noexcept
    {
        return (level / 127.0) * (level / 127.0);
    }

private:
    // Plane offsets within a kit record.
    static constexpr int tone_plane = 0x000; // 128 x u16
    static constexpr int level_plane = 0x100;
    static constexpr int pitch_plane = 0x180;
    static constexpr int group_plane = 0x200;
    static constexpr int pan_plane = 0x280;
    static constexpr int reverb_plane = 0x300;
    static constexpr int chorus_plane = 0x380;
    static constexpr int delay_plane = 0x400;
    /// Receive flags. Bit 0 is `Rx.Note Off`; the byte otherwise reads 0x10 across every kit.
    static constexpr int receive_plane = 0x480;
    /// Twelve ASCII bytes naming the kit.
    static constexpr int name_plane = 0x500;
    static constexpr int name_length = 12;

    std::int64_t kit_base_ = 0;
    std::vector<std::uint8_t> bank_row_;
    std::vector<std::uint8_t> program_map_;
    std::array<std::uint16_t, 256> kit_index_{};
    std::vector<std::uint8_t> kits_;
    int kit_count_ = 0;
};

} // namespace ts
