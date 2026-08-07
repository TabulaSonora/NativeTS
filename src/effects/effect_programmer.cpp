#include "tabulasonora/effect_programmer.hpp"

#include "dsp/fixed.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ts {
namespace {

/// The 64-bit image base every stored pointer is relative to.
constexpr std::int64_t image_base = 0x180000000LL;

/// Virtual-address to file-offset adjustment shared by every table here. The same -0x1000 the
/// `.rdata` curve tables take, confirmed by locating each table's bytes.
constexpr std::int64_t section_adjust = 0x1000;

[[nodiscard]] constexpr std::int64_t file_offset(std::int64_t virtual_address) noexcept
{
    return virtual_address - image_base - section_adjust;
}

// The GS macro parameter rows the SysEx handlers load: `sysex_reverb_macro` @ 0x180071c20 reads
// 7-byte rows from g_reverb_preset_tbl, and the chorus rows are 8 bytes wide.
constexpr std::int64_t reverb_macro_rows = file_offset(0x1819A0248);
constexpr std::int64_t chorus_macro_rows = file_offset(0x180093640);

// The pre-LPF coefficient ladder: eight (feedback, input) fixed-14 pairs. The chorus and reverb
// copies carry identical values; each unit reads its own.
constexpr std::int64_t chorus_lpf_ladder = file_offset(0x180093680);
constexpr std::int64_t reverb_damp_ladder = file_offset(0x181893738);

// Per-character program rows, reached through pointer tables (`fx_load_dsp_preset`'s
// PTR_DAT_1819a0ee0 / 1819a0cc0 / 1819a0e90 / 1819a0ab0). Ten entries: characters 0-7, one spare,
// and the boot-time program a GS reset replaces before anything sounds.
constexpr std::int64_t coefficient_pointers = file_offset(0x1819A0EE0);
constexpr std::int64_t tap_pointers = file_offset(0x1819A0CC0);
constexpr std::int64_t level_pointers = file_offset(0x1819A0E90);
constexpr std::int64_t second_tap_pointers = file_offset(0x1819A0AB0);

/// File offset of `g_delay_preset_tbl`, ten rows of ten GS delay parameters.
constexpr std::int64_t delay_preset_offset = file_offset(0x181893930);

// The EQ shelf tables, read by `fx_eq_band_preset_apply`. Each is two corner-frequency rows of 25
// gain settings, each setting three coefficients in the same fixed14 encoding as everything else —
// so the whole four-band EQ block is 300 bytes of stored answers and no arithmetic at all.
constexpr std::int64_t eq_low_table = file_offset(0x1818960B0);
constexpr std::int64_t eq_high_table = file_offset(0x1818961E0);

/// Coefficients per gain setting, and the stride of one frequency row (`0x4b` in the engine).
constexpr int eq_coefficients = 3;
constexpr int eq_row_stride = EqPresets::gain_count * eq_coefficients;

/// The ring base every GS macro program's taps are stored relative to.
constexpr int ring_base = 0x2000;

/// Roland's DELAY TIME CENTER conversion, raw 1-115 to milliseconds.
///
/// Table 16 of the published GS MIDI implementation — documentation rather than extracted binary
/// data, which is why it can live in source.
constexpr std::array<double, 115> delay_time_milliseconds{
    0.1,   0.2,   0.3,   0.4,   0.5,   0.6,   0.7,   0.8,   0.9,   1.0,   1.1,   1.2,   1.3,
    1.4,   1.5,   1.6,   1.7,   1.8,   1.9,   2.0,   2.2,   2.4,   2.6,   2.8,   3.0,   3.2,
    3.4,   3.6,   3.8,   4.0,   4.2,   4.4,   4.6,   4.8,   5.0,   5.5,   6.0,   6.5,   7.0,
    7.5,   8.0,   8.5,   9.0,   9.5,   10.0,  11.0,  12.0,  13.0,  14.0,  15.0,  16.0,  17.0,
    18.0,  19.0,  20.0,  22.0,  24.0,  26.0,  28.0,  30.0,  32.0,  34.0,  36.0,  38.0,  40.0,
    42.0,  44.0,  46.0,  48.0,  50.0,  55.0,  60.0,  65.0,  70.0,  75.0,  80.0,  85.0,  90.0,
    95.0,  100.0, 110.0, 120.0, 130.0, 140.0, 150.0, 160.0, 170.0, 180.0, 190.0, 200.0, 220.0,
    240.0, 260.0, 280.0, 300.0, 320.0, 340.0, 360.0, 380.0, 400.0, 420.0, 440.0, 460.0, 480.0,
    500.0, 550.0, 600.0, 650.0, 700.0, 750.0, 800.0, 850.0, 900.0, 950.0, 1000.0};

/// Roland's DELAY TIME RATIO conversion, raw 1-120 to percent. Table 17.
constexpr std::array<double, 120> delay_ratio_percent{
    4,   8,   13,  17,  21,  25,  29,  33,  38,  42,  46,  50,  54,  58,  63,  67,  71,  75,
    79,  83,  88,  92,  96,  100, 104, 108, 113, 117, 121, 125, 129, 133, 138, 142, 146, 150,
    154, 158, 163, 167, 171, 175, 179, 183, 188, 192, 196, 200, 204, 208, 213, 217, 221, 225,
    229, 233, 238, 242, 246, 250, 254, 258, 263, 267, 271, 275, 279, 283, 288, 292, 296, 300,
    304, 308, 313, 317, 321, 325, 329, 333, 338, 342, 346, 350, 354, 358, 363, 367, 371, 375,
    379, 383, 388, 392, 396, 400, 404, 408, 413, 417, 421, 425, 429, 433, 438, 442, 446, 450,
    454, 458, 463, 467, 471, 475, 479, 483, 488, 492, 496, 500};

/// The engine's `fixed14_to_float`: a signed 14-bit mantissa scaled by 1/8192, with the top two
/// bits selecting a shift of 0, 1, 2 or 4 — and exact zero replaced by the engine's universal
/// anti-denormal seed.
///
/// The shift runs in unsigned arithmetic because the mantissa is signed and a shifted-out sign bit
/// is what the original's `imul`-free encoding relies on.
[[nodiscard]] double fixed14(int value) noexcept
{
    const int mantissa = ((value & 0x3FFF) ^ 0x2000) - 0x2000;
    constexpr std::array<int, 4> shifts{0, 1, 2, 4};
    const int shift = shifts[static_cast<std::size_t>((value >> 14) & 3)];

    const auto shifted = static_cast<std::int32_t>(static_cast<std::uint32_t>(mantissa)
                                                   << static_cast<unsigned>(shift));
    if (shifted == 0) {
        return static_cast<double>(1e-05F);
    }
    return static_cast<double>(static_cast<float>(shifted) * 0.00012207031F);
}

/// A gain-bank target as the per-block ramp resolves it: `value / 32768`, twice, in `float` — the
/// exact arithmetic of the coefficient ramp in `fx_process_block`.
[[nodiscard]] double gain(int target) noexcept
{
    const float half = static_cast<float>(static_cast<std::int16_t>(target)) * 3.0517578e-05F;
    return static_cast<double>(half + half);
}

[[nodiscard]] constexpr int sign_extend14(int value) noexcept
{
    return ((value & 0x3FFF) ^ 0x2000) - 0x2000;
}

/// The dispatcher's tap-base decode: a positive value is a plain sample count, a negative one a
/// 12.12 fixed-point base, which is how a delay byte becomes a ring position.
///
/// The wide shift runs in `uint32_t` and is converted back, because the original's 32-bit register
/// arithmetic wraps and a signed overflow here would be undefined.
[[nodiscard]] int tap_base(int value) noexcept
{
    const int v = value & 0xFFFF;
    if (static_cast<std::int16_t>(v) >= 0) {
        return sign_extend14(v);
    }
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << 18U) >> 5;
}

/// Reads `count` little-endian `uint16` values at a file offset.
[[nodiscard]] std::vector<std::uint16_t>
read_u16(const RomImage& rom, std::int64_t offset, std::size_t count)
{
    const std::vector<std::uint8_t> bytes = rom.read(offset, count * 2);
    std::vector<std::uint16_t> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = fx::read_u16le(bytes.data() + (i * 2));
    }
    return values;
}

/// Follows one entry of a pointer table and reads the row it names.
[[nodiscard]] std::vector<std::uint16_t>
read_row(const RomImage& rom, std::int64_t pointer_table, int index, std::size_t count)
{
    const std::vector<std::uint8_t> pointer =
        rom.read(pointer_table + (static_cast<std::int64_t>(index) * 8), 8);

    std::uint64_t address = 0;
    for (int i = 7; i >= 0; --i) {
        address = (address << 8) | pointer[static_cast<std::size_t>(i)];
    }

    return read_u16(rom, file_offset(static_cast<std::int64_t>(address)), count);
}

/// Reads one EQ band's whole table: two frequency rows of 25 gain settings.
///
/// The three coefficients per setting are stored in the order the register writes take them, which
/// is not `b0, b1, a1` — `fx_eq_band_preset_apply` sends the row's first word to register 0xe7, the
/// second to 0xe6 and the third to 0xe8. Reading them straight through and naming them by position
/// is what makes the flat row come out as `{1, -a, a}`, and that identity is the proof the order is
/// right: any other assignment fails to be unity at 0 dB.
void read_eq_table(const RomImage& rom,
                   std::int64_t table,
                   std::array<std::array<EqBand, EqPresets::gain_count>, 2>& into)
{
    const std::vector<std::uint16_t> raw =
        read_u16(rom, table, static_cast<std::size_t>(2 * eq_row_stride));

    for (std::size_t frequency = 0; frequency < into.size(); ++frequency) {
        for (int gain = 0; gain < EqPresets::gain_count; ++gain) {
            const std::size_t at = (frequency * static_cast<std::size_t>(eq_row_stride))
                                   + static_cast<std::size_t>(gain * eq_coefficients);
            into[frequency][static_cast<std::size_t>(gain)] =
                EqBand{fixed14(raw[at]), fixed14(raw[at + 1]), fixed14(raw[at + 2])};
        }
    }
}

/// Reads a row as signed 16-bit, which is how the stored taps are relative to the ring base.
[[nodiscard]] std::vector<int>
read_signed_row(const RomImage& rom, std::int64_t pointer_table, int index, std::size_t count)
{
    const std::vector<std::uint16_t> raw = read_row(rom, pointer_table, index, count);
    std::vector<int> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        values[i] = static_cast<std::int16_t>(raw[i]);
    }
    return values;
}

[[nodiscard]] TankTaps tank_taps(const std::array<int, 8>& taps) noexcept
{
    return TankTaps{
        .tap10 = taps[0],
        .tap14 = taps[1],
        .tap18 = taps[2],
        .tap1c = taps[3],
        .tap20 = taps[4],
        .tap24 = taps[5],
        .tap28 = taps[6],
        .tap2c = taps[7],
    };
}

[[nodiscard]] ReverbPreset compute_reverb(const RomImage& rom, const std::uint8_t* row)
{
    const int character = row[0];
    const int pre_lpf = row[1];
    const int time = row[3];
    const int feedback = row[4];
    const int pre_delay = row[6];

    // The per-character program rows, through the loader's pointer tables. Taps are stored relative
    // to the ring base every GS macro program uses.
    const std::vector<int> stored = read_signed_row(rom, tap_pointers, character, 24);
    const std::vector<int> stored_second = read_signed_row(rom, second_tap_pointers, character, 8);
    const std::vector<std::uint16_t> coefs = read_row(rom, coefficient_pointers, character, 20);

    std::array<int, 24> tap{};
    for (std::size_t i = 0; i < tap.size(); ++i) {
        tap[i] = stored[i] + ring_base;
    }

    // Second tap row: tank A taps 4-7, then tank B taps 4-7.
    std::array<int, 4> tank_a_tail{};
    std::array<int, 4> tank_b_tail{};
    for (std::size_t i = 0; i < 4; ++i) {
        tank_a_tail[i] = stored_second[i] + ring_base;
        tank_b_tail[i] = stored_second[i + 4] + ring_base;
    }

    // Characters 6 (Delay) and 7 (Panning Delay) re-program the tank output taps from the reverb
    // time — they are the two macros whose taps a file's time parameter actually moves. The field
    // sets come from the handler's index lists (0x1819a0f40 / 0x1819a0b10 / 0x1819a0cb8) resolved
    // against the network's register wiring.
    if (character == 6) {
        const int value = ((time * 0x70) + 0x2016) & 0xFFFF;
        tank_a_tail.fill(value);
        tank_b_tail.fill(value);
        tap[23] = value; // tank B tap 3
    } else if (character == 7) {
        const int wide = ((time * 0x70) + 0x2016) & 0xFFFF;
        const int narrow = ((time * 0x38) + 0x2016) & 0xFFFF;
        tank_a_tail[1] = wide;
        tank_a_tail[3] = wide;
        tank_b_tail[1] = wide;
        tank_b_tail[3] = wide;
        tap[23] = wide; // tank B tap 3
        tank_a_tail[0] = narrow;
        tank_a_tail[2] = narrow;
        tank_b_tail[0] = narrow;
        tank_b_tail[2] = narrow;
    }

    // Tap row order, fixed by the loader's index list: four diffuser write/read pairs, then per
    // tank its two nested allpasses interleaved with that tank's first four output taps.
    ReverbPreset preset;
    for (std::size_t i = 0; i < preset.diffusers.size(); ++i) {
        preset.diffusers[i] = AllpassStage{
            .write_tap = tap[i * 2],
            .read_tap = tap[(i * 2) + 1],
            .coef_a = fixed14(coefs[i * 2]),
            .coef_b = fixed14(coefs[(i * 2) + 1]),
        };
    }

    preset.tank_allpasses = TankAllpasses{
        .a0 = {.write_tap = tap[8],
               .read_tap = tap[9],
               .coef_a = fixed14(coefs[8]),
               .coef_b = fixed14(coefs[9])},
        .a1 = {.write_tap = tap[12],
               .read_tap = tap[13],
               .coef_a = fixed14(coefs[10]),
               .coef_b = fixed14(coefs[11])},
        .b0 = {.write_tap = tap[16],
               .read_tap = tap[17],
               .coef_a = fixed14(coefs[12]),
               .coef_b = fixed14(coefs[13])},
        .b1 = {.write_tap = tap[20],
               .read_tap = tap[21],
               .coef_a = fixed14(coefs[14]),
               .coef_b = fixed14(coefs[15])},
    };

    preset.tank_a = ReverbTank{
        .taps = tank_taps({tap[10],
                           tap[11],
                           tap[14],
                           tap[15],
                           tank_a_tail[0],
                           tank_a_tail[1],
                           tank_a_tail[2],
                           tank_a_tail[3]}),
        .coef_a = fixed14(coefs[16]),
        .coef_b = fixed14(coefs[17]),
    };
    preset.tank_b = ReverbTank{
        .taps = tank_taps({tap[18],
                           tap[19],
                           tap[22],
                           tap[23],
                           tank_b_tail[0],
                           tank_b_tail[1],
                           tank_b_tail[2],
                           tank_b_tail[3]}),
        .coef_a = fixed14(coefs[18]),
        .coef_b = fixed14(coefs[19]),
    };

    // The computed registers: injection from the pre-delay byte, damping from the pre-LPF ladder,
    // and the gain-bank targets the per-block ramp converts.
    const std::vector<std::uint16_t> ladder = read_u16(rom, reverb_damp_ladder, 16);
    const int level = read_row(rom, level_pointers, character, 1)[0];

    preset.injection_tap = (pre_delay + 0x80) * 0x20;
    preset.damp_feedback = fixed14(ladder[static_cast<std::size_t>(pre_lpf) * 2]);
    preset.damp_input = fixed14(ladder[(static_cast<std::size_t>(pre_lpf) * 2) + 1]);
    preset.gain_input = gain(level << 6);
    preset.gain_injection = gain(0x4000);
    // The two delay characters price feedback from the delay-feedback byte; the rest from the
    // reverb time.
    preset.gain_feedback = character >= 6 ? gain(std::min(feedback, 0x5F) << 8)
                                          : gain(((std::min(time, 0x6C) * 0x17C) / 0x6C) << 6);
    preset.gain_output = gain(0x4000);
    return preset;
}

[[nodiscard]] ChorusPreset compute_chorus(const RomImage& rom, const std::uint8_t* row)
{
    const int pre_lpf = row[0];
    // row[1] is the return level; it is the mixer's ramp, not a network coefficient.
    const int feedback = row[2];
    const int delay = row[3];
    const int rate = row[4];
    const int depth = row[5];
    // row[6] and row[7] are the sends to the reverb and the delay; like the return level they
    // are the mixer's ramps, not coefficients. See `ChorusPreset`.

    // `chorus_apply_params`: the LFO increment and tap bases go through the dispatcher's
    // signed-14-bit decode, where a negative value is a 12.12 fixed-point base rather than a
    // sign-extended count.
    const std::vector<std::uint16_t> ladder = read_u16(rom, chorus_lpf_ladder, 16);
    const int tap_depth = (depth + 1) * 0x28;
    const int base = tap_base((delay * 3) - 0x8000);

    return ChorusPreset{
        .lfo_increment = sign_extend14(rate << 6),
        .lpf_a = fixed14(ladder[static_cast<std::size_t>(pre_lpf) * 2]),
        .lpf_b = fixed14(ladder[(static_cast<std::size_t>(pre_lpf) * 2) + 1]),
        .tap1_depth = tap_depth,
        .tap1_base = base,
        .tap2_depth = tap_depth,
        .tap2_base = base,
        .feedback = fixed14((feedback << 6) & 0xFFFF),
        .gain_write = gain(0x4000),
        // Unity, and deliberately so: the chorus **return level** is not a coefficient of this
        // network. It is `MatrixRamp`, applied in the mixer, whose `level << 8` over `1/16384` is
        // the same `raw / 64` law the live gain register shows -- 0.5 at raw 32, 1.984375 at raw
        // 127. Compiling the level in here as well renders it twice; `ff5_1_16_harvest.mid`, which
        // sends `40 01 3A` 120, is what says so out loud.
        .gain_tap = gain(0x4000),
    };
}

} // namespace

std::vector<std::array<int, EffectProgrammer::delay_preset_stride>>
EffectProgrammer::read_delay_presets(const RomImage& rom)
{
    const std::vector<std::uint8_t> bytes = rom.read(
        delay_preset_offset, static_cast<std::size_t>(delay_type_count) * delay_preset_stride);

    std::vector<std::array<int, delay_preset_stride>> presets(delay_type_count);
    for (int t = 0; t < delay_type_count; ++t) {
        for (int i = 0; i < delay_preset_stride; ++i) {
            presets[static_cast<std::size_t>(t)][static_cast<std::size_t>(i)] =
                bytes[static_cast<std::size_t>((t * delay_preset_stride) + i)];
        }
    }

    // A wrong offset would silently produce plausible junk, so check a known row.
    if (presets[0][1] != 97 || presets[0][4] != 127 || presets[0][8] != 80) {
        throw std::runtime_error("The delay preset table does not look right; the ROM's layout "
                                 "does not match the pinned build.");
    }

    return presets;
}

std::array<std::uint8_t, EffectProgrammer::reverb_row_bytes>
EffectProgrammer::reverb_macro_row(const RomImage& rom, int type)
{
    const std::vector<std::uint8_t> rows = rom.read(reverb_macro_rows, 8 * reverb_row_bytes);
    std::array<std::uint8_t, reverb_row_bytes> row{};
    const std::size_t at = static_cast<std::size_t>(std::clamp(type, 0, 7)) * reverb_row_bytes;
    std::copy_n(rows.begin() + static_cast<std::ptrdiff_t>(at), reverb_row_bytes, row.begin());
    return row;
}

std::array<std::uint8_t, EffectProgrammer::chorus_row_bytes>
EffectProgrammer::chorus_macro_row(const RomImage& rom, int type)
{
    const std::vector<std::uint8_t> rows = rom.read(chorus_macro_rows, 8 * chorus_row_bytes);
    std::array<std::uint8_t, chorus_row_bytes> row{};
    const std::size_t at = static_cast<std::size_t>(std::clamp(type, 0, 7)) * chorus_row_bytes;
    std::copy_n(rows.begin() + static_cast<std::ptrdiff_t>(at), chorus_row_bytes, row.begin());
    return row;
}

ReverbPreset EffectProgrammer::reverb_from_row(const RomImage& rom,
                                               std::span<const std::uint8_t> row)
{
    return compute_reverb(rom, row.data());
}

ChorusPreset EffectProgrammer::chorus_from_row(const RomImage& rom,
                                               std::span<const std::uint8_t> row)
{
    return compute_chorus(rom, row.data());
}

EffectPresets EffectProgrammer::compute(const RomImage& rom)
{
    const std::vector<std::uint8_t> reverb_rows = rom.read(reverb_macro_rows, 8 * 7);
    const std::vector<std::uint8_t> chorus_rows = rom.read(chorus_macro_rows, 8 * 8);

    ReverbPresets reverb;
    reverb.type_names = {"Room1", "Room2", "Room3", "Hall1", "Hall2", "Plate", "Delay", "PanDelay"};
    for (int t = 0; t < 8; ++t) {
        reverb.types.push_back(compute_reverb(rom, reverb_rows.data() + (t * 7)));
    }

    ChorusPresets chorus;
    chorus.type_names = {"Chorus1",
                         "Chorus2",
                         "Chorus3",
                         "Chorus4",
                         "FeedbackChorus",
                         "Flanger",
                         "ShortDelay",
                         "ShortDelayFB"};
    for (int t = 0; t < 8; ++t) {
        chorus.types.push_back(compute_chorus(rom, chorus_rows.data() + (t * 8)));
    }

    // The GM power-on state: the engine boots with reverb macro 4 (Hall2) and chorus macro 2
    // (Chorus3) already selected -- `fx_control_update`'s init block plants exactly those macro
    // rows into the parameter shadow -- so the defaults are those two types verbatim. A live
    // harvest confirms it: the "no macro selected" capture equals the type capture field for field.
    reverb.defaults = reverb.types[4];
    chorus.defaults = chorus.types[2];

    DelayPresets delay;
    delay.type_names = {"Delay1",
                        "Delay2",
                        "Delay3",
                        "Delay4",
                        "PanDelay1",
                        "PanDelay2",
                        "PanDelay3",
                        "PanDelay4",
                        "DelayToReverb",
                        "PanRepeat"};
    delay.time_milliseconds.assign(delay_time_milliseconds.begin(), delay_time_milliseconds.end());
    delay.ratio_percent.assign(delay_ratio_percent.begin(), delay_ratio_percent.end());
    delay.raw_presets = read_delay_presets(rom);

    EqPresets eq;
    read_eq_table(rom, eq_low_table, eq.low);
    read_eq_table(rom, eq_high_table, eq.high);

    return EffectPresets::from_parts(std::move(reverb), std::move(chorus), std::move(delay), eq);
}

} // namespace ts
