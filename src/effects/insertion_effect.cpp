#include "tabulasonora/insertion_effect.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ts {
namespace {

// ---------------------------------------------------------------------------------------------
// DLL layout. Everything here is `.rdata`, where the virtual-to-file adjustment is the same
// -0x1000 the other table readers use. Offsets are pinned to SOUND Canvas VA 1.1.6.
// ---------------------------------------------------------------------------------------------

constexpr std::int64_t image_base = 0x180000000LL;
constexpr std::int64_t section_adjust = 0x1000;

[[nodiscard]] constexpr std::int64_t file_offset(std::int64_t virtual_address) noexcept
{
    return virtual_address - image_base - section_adjust;
}

/// The EFX directory: 66 records of 0x28 bytes. The `g_fx_type_to_algo_map` symbol lands on the
/// type key 12 bytes in; this is the true record start.
constexpr std::int64_t directory_offset = file_offset(0x181895660);
constexpr int directory_records = 66;
constexpr int directory_stride = 0x28;

/// `PTR_DAT_1818953d0`: 67 pointers, one per dispatch index, to the per-algorithm register width
/// maps `fx_reg_write` consults (0 = byte, 1 = 16-bit high half, 2 = 16-bit low half).
///
/// The maps themselves live in `.data`, and some — Thru's among them — sit past the section's
/// raw bytes, in BSS: their content is *zeros*, meaning every register is a plain byte register.
/// So the read has to be section-aware where every other table is happily `.rdata`.
constexpr std::int64_t width_map_pointers_offset = file_offset(0x1818953D0);
constexpr int dispatch_count = 67;

/// `.data` for this build: virtual base, raw file base, raw size. Bytes past the raw size are
/// zero-initialised at load.
constexpr std::int64_t data_virtual = 0x181A09000;
constexpr std::int64_t data_raw = 0x1A07C00;
constexpr std::int64_t data_raw_size = 0x8600;

/// `fx_set_algo_index` reads a {u16 offset, u16 count} slice per dispatch index out of
/// `0x18199E630` and pours that many packed values from `0x18198F580` into registers
/// `0x8A`–`0x1EF`. The values are raw u32 register words — bit 15 is the scale select
/// `fx_reg_write` honours, which is why they are not plain bytes.
constexpr std::int64_t preset_slices_offset = file_offset(0x18199E630);
constexpr std::int64_t preset_data_offset = file_offset(0x18198F580);

// Parameter curves of the Overdrive/Distortion apply handler (`fx_param_apply_53a10`).
constexpr std::int64_t drive_curve_a_offset = file_offset(0x18198C160); // → reg 0xA5
constexpr std::int64_t drive_curve_b_offset = file_offset(0x18198E3C0); // → reg 0xD9
constexpr std::int64_t pan_left_offset = file_offset(0x18198BE60); // → reg 0x11B
constexpr std::int64_t pan_right_offset = file_offset(0x18198BFE0); // → reg 0x11C
constexpr std::int64_t level_curve_offset = file_offset(0x18198B360); // → regs 0x80, 0x1F0
constexpr std::int64_t amp_switch_a_offset = file_offset(0x181897364); // → reg 0x10C
constexpr std::int64_t amp_switch_b_offset = file_offset(0x181897354); // → reg 0x10B
constexpr std::int64_t low_gain_f_offset = file_offset(0x18198A5C0); // → reg 0x10F
constexpr std::int64_t low_gain_d_offset = file_offset(0x18198A9C0); // → reg 0x10D
constexpr std::int64_t low_gain_h_offset = file_offset(0x18198A8C0); // → reg 0x111
constexpr std::int64_t high_gain_f_offset = file_offset(0x18198A7C0); // → reg 0x115
constexpr std::int64_t high_gain_d_offset = file_offset(0x18198ADE0); // → reg 0x113
constexpr std::int64_t high_gain_h_offset = file_offset(0x18198ACE0); // → reg 0x117

/// The amp-simulator register bank (`chorus_load_algo_regs` — the name is historical, the routine
/// programs any 0x13-register bank): the register numbers live at `0x181896F50` and the per-type
/// values in sixteen four-entry tables, indexed by amp type 0–3.
constexpr std::int64_t amp_bank_registers_offset = file_offset(0x181896F50);
constexpr int amp_bank_register_count = 0x13;

// The EFX Reverb (`fx_process` @ 0x180056560, the record's own apply handler). Its Character
// parameter programs a register bank — two 16-bit registers at `0x181897148`, seventeen byte
// registers at `0x18189714C` — first with the defaults at `0x18198EE18`, then with one of six
// per-character value tables (2 × u16, 16 low bytes, and a final byte for register 0x9F), and
// pours a 32-entry tap program into the delay-line index array the algorithm reads.
constexpr std::int64_t reverb_bank_regs16_offset = file_offset(0x181897148);
constexpr std::int64_t reverb_bank_regs8_offset = file_offset(0x18189714C);
constexpr std::int64_t reverb_bank_defaults_offset = file_offset(0x18198EE18);
constexpr std::int64_t reverb_default_taps_offset = file_offset(0x18198F320);
constexpr std::int64_t reverb_char_tap_pointers = file_offset(0x181897178);
constexpr std::int64_t reverb_char_value_pointers = file_offset(0x1818971B0);
constexpr std::int64_t reverb_fallback_taps_offset = file_offset(0x18198F4B0);
constexpr std::int64_t reverb_fallback_values_offset = file_offset(0x18198F530);
constexpr std::int64_t reverb_lpf_curve_offset = file_offset(0x18198B8E0); // → lfo5 @ 0xA9
constexpr std::int64_t reverb_time_curve_offset = file_offset(0x18198F1D0); // → 0xF5, 0x137
constexpr std::int64_t reverb_feedback_a_offset = file_offset(0x18198BAE0); // → 0xF1, 0x133
constexpr std::int64_t reverb_feedback_b_offset = file_offset(0x18198BBE0); // → 0xEE, 0x130
constexpr std::int64_t reverb_p16_a_offset = file_offset(0x18198B5E0); // → 0x18C, 0x1B4
constexpr std::int64_t reverb_p16_b_offset = file_offset(0x18198B660); // → 0x18B, 0x1B3

// OD / OD2 (`fx_param_apply_51be0`). Two overdrive chains side by side, each with its own type,
// drive, amp simulator, pan and level, mixed at the end. Selecting a chain's OD type programs a
// bank of nineteen byte registers from these tables, indexed by the type (0 or 1); both chains
// read the same tables and write different registers.
constexpr std::array<std::int64_t, 19> od2_bank_tables{
    0x18198E0A8, 0x18198E0A8, 0x18198E464, 0x18198E440, 0x18198E3BC, 0x18198E3B8, 0x18198E468,
    0x18198E3B0, 0x18198E45C, 0x18198E460, 0x18198E454, 0x18198E458, 0x18198E470, 0x18198E444,
    0x18198E46C, 0x18198E448, 0x18198E3B4, 0x18198E44C, 0x18198E450,
};
/// The register each of those tables writes, as `fx_reg_write` indices (register minus 0x80).
constexpr std::array<int, 19> od2_bank_registers_a{
    0x2D, 0x30, 0x16, 0x17, 0x18, 0x1B, 0x1A, 0x19, 0x1E, 0x1D,
    0x1C, 0x4D, 0x4C, 0x4B, 0x50, 0x4F, 0x4E, 0x51, 0x52,
};
constexpr std::array<int, 19> od2_bank_registers_b{
    0xAF, 0xB2, 0x98, 0x99, 0x9A, 0x9D, 0x9C, 0x9B, 0xA0, 0x9F,
    0x9E, 0xCF, 0xCE, 0xCD, 0xD2, 0xD1, 0xD0, 0xD3, 0xD4,
};
/// The second drive curve has two variants here, picked by the chain's OD type rather than fixed:
/// `0x18198E3C0` is Overdrive's, `0x18198E480` the other one.
constexpr std::int64_t od2_drive_alt_offset = file_offset(0x18198E480);
// Stereo EQ (`fx_param_apply_48680`). Each shelf has two frequency settings, and the setting
// picks which triple of gain tables the band reads — the shared ones the other types use, or
// these alternates.
constexpr std::int64_t eq_low_alt_f_offset = file_offset(0x18198B1E0);
constexpr std::int64_t eq_low_alt_d_offset = file_offset(0x18198B0E0);
constexpr std::int64_t eq_low_alt_h_offset = file_offset(0x18198A6C0);
constexpr std::int64_t eq_high_alt_f_offset = file_offset(0x18198AEE0);
constexpr std::int64_t eq_high_alt_d_offset = file_offset(0x18198AFE0);
constexpr std::int64_t eq_high_alt_h_offset = file_offset(0x18198AAC0);

// The four-argument bank loader (`reverb_load_algo_regs` @ 0x1800053E0) the EQ's two mid bands
// and several other types program through. A band picks one of seventeen coefficient tables by
// its frequency, then a five-byte row inside it by gain and Q, and the row's fifth byte carries
// four scale flags for the four registers the first four bytes fill.
constexpr std::array<std::int64_t, 17> bank_coef_tables{
    0x181989940, 0x181988F40, 0x181988A40, 0x1819891C0, 0x181989BC0, 0x1819882A0,
    0x181989440, 0x181988CC0, 0x18198A0C0, 0x1819887C0, 0x181989E40, 0x18198A340,
    0x181988020, 0x181987DA0, 0x181987B20, 0x181988520, 0x1819896C0,
};
/// Index 7 falls to the same table the switch uses as its default.
constexpr std::size_t bank_default_table = 7;
constexpr int bank_table_rows = 125; ///< (gain 0-24) x (Q 0-4)
constexpr int bank_row_bytes = 5;
/// The byte pair each frequency also carries: value, then a flag in the high bit of the next.
constexpr std::int64_t bank_pair_offset = file_offset(0x181988798);

/// The EQ's four register banks — the two mid bands, each programmed twice (once per channel).
constexpr std::array<std::int64_t, 4> eq_mid_banks{
    0x1818967E0, 0x181896768, 0x181896798, 0x181896778,
};
constexpr int bank_register_count = 7;

// Rotary (`fx_param_apply_57bc0`). Its own three curves on top of the shared gain and level ones.
constexpr std::int64_t rotary_rate_offset = file_offset(0x18198CA60);   // u16 -> 0x139, 0xBE
constexpr std::int64_t rotary_spread_offset = file_offset(0x18198F040); // u8  -> 0xF2, 0x126
constexpr std::int64_t rotary_speed_offset = file_offset(0x18198B7E0);  // u16 -> 0xB6/0xBA, 0x131/0x135

/// The two chains' amp-simulator register banks (`chorus_load_algo_regs` takes the bank).
constexpr std::int64_t od2_amp_bank_a_offset = file_offset(0x181896E60);
constexpr std::int64_t od2_amp_bank_b_offset = file_offset(0x181896E88);

/// The tap index array `DAT_181a0f108`–`0f18c`: slot 0 is pinned to 0x6001 by the programmer
/// (one sample behind the fixed write at float index 0x6000), slot 1 is unused, and slots 2–33
/// carry the 32 unpacked u16 taps.
constexpr int reverb_tap_count = 34;
constexpr int reverb_write_index = 0x6000;

constexpr float anti_denormal = 1e-05F;

// ---------------------------------------------------------------------------------------------
// Register file — `fx_reg_write` @ 0x1800898D0 and its wrappers, transcribed.
//
// A register is a byte with a u32 word behind it whose upper bits carry flags — bit 15 selects
// the wide scale — and the conversion to float depends on the current algorithm's width map.
// "Slew" writes step one unit per iteration *inside one call*, so their end state is the target;
// only the end state matters to an offline render and only it is reproduced.
// ---------------------------------------------------------------------------------------------

class RegisterFile {
public:
    static constexpr int count = InsertionEffect::register_count;

    /// Coefficients, `g_fx_coef_f32`. One slot of headroom: a width-1 write lands one slot on.
    std::array<float, count + 1> coef{};

    void set_width_map(const std::uint8_t* map) noexcept { width_map_ = map; }

    /// `fx_reg_write`: zero-based index, raw register word.
    void write_index(int index, std::uint32_t value) noexcept
    {
        if (index < 0 || index >= count) {
            return;
        }
        const std::uint8_t kind = width_map_ == nullptr ? 0xFF : width_map_[index];
        const float scale8 = (value & 0x8000U) != 0 ? 0.03125F : 0.0078125F;
        const float scale16 = (value & 0x8000U) != 0 ? 0.00012207031F : 3.0517578e-05F;
        const auto low = static_cast<std::int8_t>(value & 0xFFU);
        shadow_byte_[static_cast<std::size_t>(index)] = low;
        if (kind == 0) {
            coef[static_cast<std::size_t>(index)] =
                low != 0 ? static_cast<float>(low) * scale8 : anti_denormal;
        } else if (kind == 1) {
            // This register is the high byte; the pair's coefficient lands one slot on, where the
            // low byte lives.
            const auto pair = static_cast<std::int16_t>(
                static_cast<std::uint8_t>(shadow_byte_[static_cast<std::size_t>(index) + 1])
                + low * 0x100);
            coef[static_cast<std::size_t>(index) + 1] =
                pair != 0 ? static_cast<float>(pair) * scale16 : anti_denormal;
        } else if (kind == 2) {
            const auto pair = static_cast<std::int16_t>(
                (index > 0 ? shadow_byte_[static_cast<std::size_t>(index) - 1] : 0) * 0x100
                + static_cast<int>(value & 0xFFU));
            coef[static_cast<std::size_t>(index)] =
                pair != 0 ? static_cast<float>(pair) * scale16 : anti_denormal;
        }
        // Any other kind stores the byte without producing a coefficient.
    }

    /// `fx_reg_write_slew`: engine register number (0x80-based), byte target. The word's upper
    /// bits — including the scale flag the preset planted — ride along.
    void write_slew(int reg, std::uint8_t target) noexcept
    {
        const std::uint32_t merged = (mirror(reg) & ~0xFFU) | target;
        write_index(reg - 0x80, merged);
        mirror(reg) = merged;
    }

    /// `fx_reg_write16`: high byte to `reg`, low byte to `reg + 1`.
    void write16(int reg, std::uint16_t value) noexcept
    {
        const std::uint32_t high = (value >> 8) | (mirror(reg) & ~0xFFU);
        const std::uint32_t low = (mirror(reg + 1) & ~0xFFU) | (value & 0xFFU);
        write_index(reg - 0x80, high);
        mirror(reg) = high;
        write_index(reg - 0x7F, low);
        mirror(reg + 1) = low;
    }

    /// `fx_reg_write_pair_ordered` sorts the two writes for the slew's benefit; end state is both.
    void write_pair(int reg_a, std::uint8_t a, int reg_b, std::uint8_t b) noexcept
    {
        write_slew(reg_a, a);
        write_slew(reg_b, b);
    }

    /// `fx_reg_write_lfo5` @ 0x180062100: five 3-bit fields into bits 16-18 of registers
    /// `reg+1`–`reg+5`, and bit 15 of the value into bit 16 of `reg+6`. The low bytes ride
    /// through unchanged, so the audible state lives in the mirror words, exactly as it does in
    /// the engine.
    void write_lfo5(int reg, std::uint16_t value) noexcept
    {
        for (int k = 0; k < 5; ++k) {
            const std::uint32_t field =
                (static_cast<std::uint32_t>(value >> (3 * k)) << 16) & 0x70000U;
            const std::uint32_t merged = (mirror(reg + 1 + k) & ~0x70000U) | field;
            write_index(reg + 1 + k - 0x80, merged);
            mirror(reg + 1 + k) = merged;
        }
        const std::uint32_t merged = (mirror(reg + 6) & 0xFFFEFFFFU)
                                     | ((static_cast<std::uint32_t>(value) & 0x8000U) << 1);
        write_index(reg + 6 - 0x80, merged);
        mirror(reg + 6) = merged;
    }

    /// A write that also sets the wide-scale flag from a table bit: the low byte and bit 15 are
    /// cleared, the flag reinstated, and the value merged in.
    void write_flagged(int reg, std::uint8_t value, bool flag) noexcept
    {
        std::uint32_t merged = mirror(reg) & 0xFFFF7F00U;
        if (flag) {
            merged |= 0x8000U;
        }
        merged |= value;
        write_index(reg - 0x80, merged);
        mirror(reg) = merged;
    }

    /// A raw-index byte write: the value merges into the mirror's low byte, index unbiased.
    void write_index_byte(int index, std::uint8_t value) noexcept
    {
        const std::uint32_t merged = (mirror(index + 0x80) & ~0xFFU) | value;
        write_index(index, merged);
        mirror(index + 0x80) = merged;
    }

    /// The byte-merge write the bank programmer uses: low byte into the mirror word, whole word
    /// through `fx_reg_write`. The same end state as a slew, kept separate to mirror the callers.
    void write_low(int reg, std::uint8_t low) noexcept { write_slew(reg, low); }

    /// The preset fill of `fx_set_algo_index`: registers 0x8A–0x1EF from the packed slice, zeros
    /// once the slice runs out, raw words into the mirror.
    void fill_preset(std::span<const std::uint32_t> slice) noexcept
    {
        std::size_t next = 0;
        for (int reg = 0x8A; reg < 0x1F0; ++reg) {
            const std::uint32_t value = next < slice.size() ? slice[next++] : 0;
            write_index(reg - 0x80, value);
            mirror(reg) = value;
        }
    }

    /// The "no effect" reset: the whole coefficient area to the anti-denormal zero.
    void clear_coefficients() noexcept { coef.fill(anti_denormal); }

    /// The startup program for the block's I/O conditioner rows, registers 0x80–0x89 and
    /// 0x1F0–0x1FF. Captured from the live engine (`scdec efxdump`) and identical across every
    /// type selected: these words carry the routing scale flags — bit 15 of 0x83/0x85 and their
    /// 0x1F3/0x1F5 twins is what makes the normal routing value 0x7F mean ×3.97, which is the
    /// block's whole make-up gain. The upper bytes ride along so later merges preserve them.
    void seed_startup() noexcept
    {
        static constexpr std::array<std::pair<std::uint16_t, std::uint32_t>, 20> words{{
            {0x000, 0x0020787FU}, {0x001, 0x00C87E7FU}, {0x002, 0x00000000U},
            {0x003, 0x0028F57FU}, {0x004, 0x00287520U}, {0x005, 0x00208120U},
            {0x006, 0x0008757FU}, {0x007, 0x00087E00U}, {0x008, 0x00000000U},
            {0x009, 0x00C85800U}, {0x170, 0x0020797FU}, {0x171, 0x00C87E7FU},
            {0x172, 0x00000000U}, {0x173, 0x0028F57FU}, {0x174, 0x00287520U},
            {0x175, 0x00208120U}, {0x176, 0x0008757FU}, {0x177, 0x00087E00U},
            {0x178, 0x00000000U}, {0x179, 0x00C85800U},
        }};
        for (const auto& [index, word] : words) {
            write_index(index, word);
            mirror(index + 0x80) = word;
        }
        // The one signed value in the rows: reg 0x1FA, −3/128.
        write_index(0x17A, 0x00C86FFDU);
        mirror(0x1FA) = 0x00C86FFDU;
        for (int index = 0x17B; index < 0x180; ++index) {
            write_index(index, 0);
        }
    }

private:
    [[nodiscard]] std::uint32_t& mirror(int reg) noexcept
    {
        return mirror_[static_cast<std::size_t>(reg - 0x80)];
    }

    const std::uint8_t* width_map_ = nullptr;
    std::array<std::int8_t, count + 2> shadow_byte_{};
    std::array<std::uint32_t, count + 2> mirror_{};
};

// ---------------------------------------------------------------------------------------------
// The delay lines. `fx_delayline_wrap` moves each buffer's base down one float per sample and
// relocates the block when it hits the bottom; the relocation exists only to keep C pointers
// contiguous, so a power-of-two ring with a decrementing base is the same machine. An algorithm
// addresses state and delays alike as taps at fixed byte offsets from the base: a slot written at
// offset X reads back at X + 4 one sample later.
// ---------------------------------------------------------------------------------------------

class Tape {
public:
    /// `fx_buffer_alloc` seeds every cell with the anti-denormal 1e-05, not zero.
    explicit Tape(int floats)
        : ring_(static_cast<std::size_t>(floats), anti_denormal), mask_(floats - 1)
    {
    }

    [[nodiscard]] float& at(int byte_offset) noexcept
    {
        return ring_[static_cast<std::size_t>((base_ + (byte_offset >> 2)) & mask_)];
    }

    /// A tap by float index, the way the reverb's tap program addresses buffer B.
    [[nodiscard]] float& tap(int float_index) noexcept
    {
        return ring_[static_cast<std::size_t>((base_ + float_index) & mask_)];
    }

    void step() noexcept { base_ = (base_ - 1) & mask_; }

    void clear() noexcept { std::fill(ring_.begin(), ring_.end(), anti_denormal); }

private:
    std::vector<float> ring_;
    int mask_;
    int base_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Algorithm processors — straight transcriptions, one engine sample per call. Inputs arrive
// already doubled and outputs are halved by the caller, matching `fx_process_block`. Stores that
// nothing in the function reads back (the 0x1D4 scratch slot) are kept: another tap may read them
// a sample later, and proving otherwise per algorithm is not worth the risk.
// ---------------------------------------------------------------------------------------------

/// `fx_algo_thru` @ 0x18003D220: a one-sample-delayed, gain-scaled pass. The right channel runs
/// registers 0x80–0x8B, the left 0x1F0–0x1F6 with the cross through slot 0x1F8.
void thru_sample(float in_left,
                 float in_right,
                 float& out_left,
                 float& out_right,
                 Tape& a,
                 const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float right = c[0] * a.at(0x1E0) + 1e-08F;
    const float right_in = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = right;
    a.at(0x1D4) = right_in;
    right = c[3] * right + 1e-08F;
    a.at(0x1D4) = right;
    a.at(0x1F8) = c[4] * right_in + 1e-08F;
    out_right = c[6] * right + 1e-08F;
    a.at(0x1DC) = c[0xB] * a.at(0x1F8) + 1e-08F;
    a.at(0x1E4) = a.at(0x1FC) * c[10] + 1e-08F;
    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// `fx_algo_overdrive` @ 0x18002DAA0 and `fx_algo_distortion` @ 0x180018560 — the two decompiled
/// functions are the same dataflow to the instruction (verified by diff); only their presets
/// differ. Input conditioner, tone shelves, a gain ladder into the clipper at slot 0x140, the
/// amp-simulator filter bank, and the output stage.
void overdrive_sample(float in_left,
                      float in_right,
                      float& out_left,
                      float& out_right,
                      Tape& a,
                      const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float f3 = c[0] * a.at(0x1E0) + 1e-08F;
    float f5 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f3;
    a.at(0x1D4) = f5;
    f3 = f3 * c[3] + 1e-08F;
    a.at(0x1D4) = f3;
    a.at(0x1F8) = f5 * c[4] + 1e-08F;
    out_right = c[6] * f3 + 1e-08F;
    float f4 = a.at(0x1FC) * c[10] + c[0xB] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f4;
    f3 = c[0xE] * f4 + 1e-08F;
    a.at(0x1D4) = f3;
    f3 = c[0x10] * a.at(0x1C) + f3 * 1e-05F + f3 * c[0x12] + 1e-08F;
    a.at(0x40) = f3;
    a.at(0x18) = f3 * 1e-05F + c[0x14] * a.at(0x1C) + f3 * c[0x16] + 1e-08F;
    float f7 = f3 * c[0x1A] + c[0x1B] * a.at(0x44) + a.at(0x48) * c[0x1C] + 1e-08F;
    a.at(0x44) = f7;
    float f6 = c[0x1D] * a.at(0x4C) + c[0x1E] * a.at(0x48) + c[0x1F] * f7 + 1e-08F;
    a.at(0x48) = f6;
    f6 = c[0x20] * a.at(0x50) + c[0x21] * a.at(0x4C) + c[0x22] * f6 + 1e-08F;
    a.at(0x4C) = f6;
    f6 = c[0x25] * f6 + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = c[0x28] * f6 + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = c[0x2B] * f6 + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = c[0x2E] * f6 + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = c[0x31] * f6 + 1e-08F;
    a.at(0x1D4) = f6;
    f4 = c[0x34] * f6 + 1e-08F;
    // The clipper, with the engine's exact branch shape.
    f6 = 1.0F;
    if (f4 <= 1.0F) {
        f6 = f4;
        if (f4 < -1.0F) {
            f6 = -1.0F;
        }
    }
    a.at(0x140) = f6;
    float t = c[0x37] * a.at(0x140);
    if (t <= 0.0F) {
        t = -t;
    }
    t = c[0x38] * 0.5F + t;
    a.at(0x1D4) = t + 1e-08F;
    if (t < 0.0F) {
        a.at(0x1D4) = c[0x3E] * 1.52588e-05F + 1e-08F;
    }
    f4 = c[0x42] * a.at(0x1D4) + c[0x43] * 0.5F + 1e-08F;
    a.at(0x1D4) = f4;
    const float knee = c[0x46] * f4 + c[0x45] * 0.5F + 1e-08F;
    f3 = (a.at(0x140) * 1e-05F - knee * a.at(0x140)) + 1e-08F;
    a.at(0x1D4) = f3;
    a.at(0x60) = c[0x4E] * f3 + 1e-08F;
    f6 = c[0x50] * a.at(0x64) + a.at(0x68) * c[0x4F] + c[0x51] * a.at(0x60) + 1e-08F;
    a.at(0x64) = f6;
    f3 = c[0x52] * a.at(0x6C) + a.at(0x68) * c[0x53] + c[0x54] * f6 + c[0x55] * a.at(0x70)
         + c[0x56] * a.at(0x74) + 1e-08F;
    a.at(0x6C) = f3;
    const float d110 = a.at(0x110);
    a.at(0x80) = c[0x59] * f3 + 1e-08F;
    f3 = a.at(0x80);
    float f8 = (c[0x5B] + 1e-05F) * a.at(0x84) + f3 * 1e-05F + f3 * c[0x5D] + a.at(0x88) * 1e-05F
               + a.at(0x88) * c[0x5F] + 1e-08F;
    a.at(0x84) = f8;
    const float d120 = a.at(0x120);
    f8 = c[0x63] * f8 + d110 * c[0x64] + 1e-08F;
    a.at(0x114) = (c[0x61] + 1e-05F) * d110 + c[0x62] * a.at(0x118) + 1e-08F;
    a.at(0x1D4) = f8;
    a.at(0x8C) = c[0x67] * f8 + c[0x66] * a.at(0x114) + 1e-08F;
    const float f8c = a.at(0x8C);
    a.at(0x10C) = f8c * 1e-05F + d110 * c[0x69] + f8c * c[0x6B] + 1e-08F;
    const float f9 = c[0x79] * a.at(0x128) + (c[0x78] + 1e-05F) * d120 + 1e-08F;
    const float f8d = c[0x6E] * a.at(0x10C) + (c[0x6D] + 1e-05F) * f8c + a.at(0x90) * 1e-05F
                      + a.at(0x90) * c[0x70] + a.at(0x94) * 1e-05F + a.at(0x94) * c[0x72]
                      + a.at(0x98) * 1e-05F + a.at(0x98) * c[0x74] + a.at(0x9C) * 1e-05F
                      + a.at(0x9C) * c[0x76] + 1e-08F;
    a.at(0x94) = f8d;
    a.at(0x124) = f9;
    f5 = d120 * c[0x7B] + c[0x7A] * f8d + 1e-08F;
    a.at(0x1D4) = f5;
    f7 = c[0x7E] * f5 + c[0x7D] * a.at(0x124) + 1e-08F;
    a.at(0x1D4) = f7;
    a.at(0x9C) = f9;
    a.at(0x11C) = f7 * 1e-05F + d120 * c[0x80] + f7 * c[0x82] + 1e-08F;
    f7 = (c[0x87] + 1e-05F) * f9 + a.at(0x108) * 1e-05F + a.at(0x108) * c[0x89] + 1e-08F;
    a.at(0x1D4) = f7;
    const float d188 = a.at(0x188);
    a.at(0x180) = c[0x8C] * f7 + f3 * c[0x8B] + 1e-08F;
    f4 = (c[0x8E] + 1e-05F) * a.at(0x184) + a.at(0x180) * 1e-05F + a.at(0x180) * c[0x90]
         + d188 * 1e-05F + d188 * c[0x92] + 1e-08F;
    a.at(0x184) = f4;
    f4 = (c[0x94] + 1e-05F) * d188 + f4 * 1e-05F + f4 * c[0x96] + a.at(0x18C) * 1e-05F
         + a.at(0x18C) * c[0x98] + 1e-08F;
    a.at(0x188) = f4;
    a.at(0x1DC) = f4 * c[0x9C] + 1e-08F;
    a.at(0x1E4) = f4 * c[0x9B] + 1e-08F;
    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// `fx_algo_reverb` @ 0x180036230: the EFX Reverb. The same input conditioner and output stage
/// as the others around a tank built on buffer B — the input joins at the fixed write index
/// 0x6000, and everything else reads and writes through the 34-slot tap program the Character
/// parameter loads. `taps` are float indices into B; buffer A carries the conditioner and the
/// four output shelving sections.
void reverb_sample(float in_left,
                   float in_right,
                   float& out_left,
                   float& out_right,
                   Tape& a,
                   Tape& b,
                   const std::int32_t* t,
                   const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float f5 = c[0] * a.at(0x1E0) + 1e-08F;
    float f7 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f5;
    a.at(0x1D4) = f7;
    f5 = c[3] * f5 + 1e-08F;
    a.at(0x1D4) = f5;
    a.at(0x1F8) = f7 * c[4] + 1e-08F;
    out_right = c[6] * f5 + 1e-08F;
    float f6 = c[10] * a.at(0x1FC) + c[0xB] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = (c[0xF] + 1e-05F) * f6 + a.at(0x20) * c[0x10] + 1e-08F;
    a.at(0x1D4) = f6;
    a.at(0x1C) = f6 * c[0x14] + f6 * 1e-05F + a.at(0x20) * c[0x15] + 1e-08F;
    a.at(0x24) = f6 * c[0x1A] + f6 * 1e-05F + a.at(0x28) * 1e-05F + a.at(0x28) * c[0x1C] + 1e-08F;
    f6 = c[0x1F] * a.at(0x24) + 1e-08F;
    a.at(0x1D4) = f6;
    b.tap(reverb_write_index) = c[0x22] * f6 + 1e-08F;
    a.at(0x1E8) = b.tap(t[3]);
    a.at(0x1EC) = b.tap(t[0]);
    f7 = c[0x35] * a.at(0x1EC) + c[0x2F] * a.at(0x1E8) + 1e-08F;
    b.tap(t[2]) = f7;
    a.at(0x1D4) = f7;
    float carry = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[5]);
    f7 = c[0x42] * a.at(0x1E8) + c[0x3A] * carry + c[0x39] * f7 + 1e-08F;
    b.tap(t[4]) = f7;
    a.at(0x1D4) = f7;
    carry = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[7]);
    f7 = c[0x4F] * a.at(0x1E8) + c[0x46] * f7 + c[0x47] * carry + 1e-08F;
    b.tap(t[6]) = f7;
    a.at(0x1D4) = f7;
    carry = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[9]);
    f6 = c[0x5C] * a.at(0x1E8) + c[0x53] * f7 + c[0x54] * carry + 1e-08F;
    b.tap(t[8]) = f6;
    a.at(0x1D4) = f6;
    a.at(0x80) = c[0x60] * f6 + c[0x61] * a.at(0x1E8) + 1e-08F;
    f6 = b.tap(t[21]);
    a.at(0x1E8) = f6;
    float f8 = (c[0x6F] + 1e-05F) * a.at(0x44) + f6 * c[0x70] + f6 * 1e-05F + f6 * c[0x72]
               + 1e-08F;
    a.at(0x40) = f8;
    a.at(0x1E8) = b.tap(t[11]);
    f6 = f8 * 1e-05F + c[0x64] * a.at(0x80) + f8 * c[0x76] + c[0x83] * a.at(0x1E8) + 1e-08F;
    b.tap(t[10]) = f6;
    a.at(0x1D4) = f6;
    b.tap(t[12]) = c[0x88] * f6 + c[0x89] * a.at(0x1E8) + 1e-08F;
    a.at(0x1E8) = b.tap(t[15]);
    carry = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[17]);
    f6 = c[0x9C] * a.at(0x1E8) + c[0x96] * carry + 1e-08F;
    b.tap(t[16]) = f6;
    a.at(0x1D4) = f6;
    b.tap(t[18]) = c[0xA0] * f6 + c[0xA1] * a.at(0x1E8) + 1e-08F;
    float f5b = b.tap(t[33]);
    a.at(0x1E8) = f5b;
    f5b = (c[0xB1] + 1e-05F) * a.at(0x4C) + f5b * c[0xB2] + f5b * 1e-05F + f5b * c[0xB4] + 1e-08F;
    a.at(0x48) = f5b;
    a.at(0x1E8) = b.tap(t[23]);
    f6 = f5b * 1e-05F + c[0xA6] * a.at(0x80) + f5b * c[0xB8] + c[0xC5] * a.at(0x1E8) + 1e-08F;
    b.tap(t[22]) = f6;
    a.at(0x1D4) = f6;
    b.tap(t[24]) = c[0xCA] * f6 + c[0xCB] * a.at(0x1E8) + 1e-08F;
    a.at(0x1E8) = b.tap(t[27]);
    carry = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[29]);
    f6 = c[0xDE] * a.at(0x1E8) + c[0xD8] * carry + 1e-08F;
    b.tap(t[28]) = f6;
    a.at(0x1D4) = f6;
    b.tap(t[30]) = c[0xE2] * f6 + c[0xE3] * a.at(0x1E8) + 1e-08F;
    a.at(0x1E8) = b.tap(t[13]);
    carry = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[19]);
    float f7b = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[25]);
    const float f2 = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[31]);
    f8 = c[0xFA] * f7b + c[0xF4] * carry + c[0x100] * f2 + c[0x106] * a.at(0x1E8) + 1e-08F;
    a.at(0x1D4) = f8;
    f8 = c[0x109] * f8 + 1e-08F;
    a.at(0x1D4) = f8;
    a.at(0x180) = c[0x10C] * f8 + c[0x10B] * a.at(0x1FC) + 1e-08F;
    a.at(0x1E8) = b.tap(t[14]);
    carry = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[20]);
    f7b = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[26]);
    const float f2b = a.at(0x1E8);
    a.at(0x1E8) = b.tap(t[32]);
    f8 = c[0x122] * f7b + c[0x11C] * carry + c[0x128] * f2b + c[0x12E] * a.at(0x1E8) + 1e-08F;
    a.at(0x1D4) = f8;
    f5 = c[0x131] * f8 + 1e-08F;
    a.at(0x1D4) = f5;
    f8 = a.at(0x188);
    a.at(0x190) = c[0x134] * f5 + c[0x133] * a.at(0x1F8) + 1e-08F;
    const float f5c = a.at(0x198);
    f7 = (c[0x139] + 1e-05F) * a.at(0x184) + a.at(0x180) * 1e-05F + a.at(0x180) * c[0x13B]
         + f8 * 1e-05F + f8 * c[0x13D] + 1e-08F;
    a.at(0x184) = f7;
    f6 = (c[0x13F] + 1e-05F) * f8 + f7 * 1e-05F + f7 * c[0x141] + a.at(0x18C) * 1e-05F
         + a.at(0x18C) * c[0x143] + 1e-08F;
    a.at(0x188) = f6;
    a.at(0x1E4) = f6;
    f8 = (c[0x149] + 1e-05F) * a.at(0x194) + a.at(0x190) * 1e-05F + a.at(0x190) * c[0x14B]
         + f5c * 1e-05F + f5c * c[0x14D] + 1e-08F;
    a.at(0x194) = f8;
    f6 = (c[0x14F] + 1e-05F) * f5c + f8 * 1e-05F + f8 * c[0x151] + a.at(0x19C) * 1e-05F
         + a.at(0x19C) * c[0x153] + 1e-08F;
    a.at(0x198) = f6;
    a.at(0x1DC) = f6;
    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// `fx_algo_overdrive_1_par_overdrive_2` @ 0x18002F450: OD / OD2, two overdrive chains running
/// side by side off the same input and mixed at the end, each with its own drive, clipper, amp
/// simulator and tone stack. The chains are not quite symmetric in one respect worth keeping:
/// chain one reads the conditioned input one sample late (slot 0x1FC, where the conditioner wrote
/// 0x1F8 last sample) while chain two reads it in the same sample. That is the engine's, not a
/// transcription slip.
void od_od2_sample(float in_left,
                   float in_right,
                   float& out_left,
                   float& out_right,
                   Tape& a,
                   const float* c) noexcept
{
    const auto clamp_unit = [](float x) noexcept {
        const float v = x + 1e-08F;
        return v > 1.0F ? 1.0F : (v < -1.0F ? -1.0F : v);
    };

    a.at(0x1F8) = in_right;
    float f4 = c[0] * a.at(0x1E0) + 1e-08F;
    const float f6 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1D4) = f6;
    f4 = c[3] * f4 + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1F8) = c[4] * f6 + 1e-08F;
    out_right = c[6] * f4 + 1e-08F;

    // ---- chain one -------------------------------------------------------------------------
    float x = a.at(0x1FC) * c[0x0A] + 1e-08F;
    a.at(0x1D4) = x;
    x = x * 1e-05F + a.at(0x1C) * c[0x0C] + x * c[0x0E] + 1e-08F;
    a.at(0x28) = x;
    a.at(0x18) = a.at(0x1C) * c[0x10] + x * 1e-05F + x * c[0x12] + 1e-08F;
    float t1 = x * c[0x16] + c[0x17] * a.at(0x2C) + a.at(0x30) * c[0x18] + 1e-08F;
    a.at(0x2C) = t1;
    float t2 = c[0x1A] * a.at(0x30) + a.at(0x34) * c[0x19] + c[0x1B] * t1 + 1e-08F;
    a.at(0x30) = t2;
    t2 = c[0x1C] * a.at(0x38) + c[0x1D] * a.at(0x34) + c[0x1E] * t2 + 1e-08F;
    a.at(0x34) = t2;
    for (const int k : {0x21, 0x24, 0x27, 0x2A, 0x2D}) {
        t2 = c[k] * t2 + 1e-08F;
        a.at(0x1D4) = t2;
    }
    a.at(0x1E8) = clamp_unit(c[0x30] * t2);
    float shape = c[0x33] * a.at(0x1E8);
    if (shape <= 0.0F) {
        shape = -shape;
    }
    shape = c[0x34] * 0.5F + shape;
    a.at(0x1D4) = shape + 1e-08F;
    if (shape < 0.0F) {
        a.at(0x1D4) = c[0x3A] * 1.52588e-05F + 1e-08F;
    }
    float f5 = c[0x3E] * a.at(0x1D4) + c[0x3F] * 0.5F + 1e-08F;
    a.at(0x1D4) = f5;
    float knee = c[0x42] * f5 + c[0x41] * 0.5F + 1e-08F;
    x = (a.at(0x1E8) * 1e-05F - knee * a.at(0x1E8)) + 1e-08F;
    a.at(0x1D4) = x;
    a.at(0x3C) = c[0x4A] * x + 1e-08F;
    t2 = a.at(0x44) * c[0x4B] + c[0x4C] * a.at(0x40) + c[0x4D] * a.at(0x3C) + 1e-08F;
    a.at(0x40) = t2;
    x = a.at(0x44) * c[0x4F] + c[0x4E] * a.at(0x48) + c[0x50] * t2 + c[0x51] * a.at(0x4C)
        + c[0x52] * a.at(0x50) + 1e-08F;
    a.at(0x48) = x;
    const float d110 = a.at(0x110);
    a.at(0x80) = c[0x55] * x + 1e-08F;
    const float chain_a_tap = a.at(0x80);
    float f8 = (c[0x57] + 1e-05F) * a.at(0x84) + chain_a_tap * 1e-05F + chain_a_tap * c[0x59]
               + a.at(0x88) * 1e-05F + a.at(0x88) * c[0x5B] + 1e-08F;
    a.at(0x84) = f8;
    const float d120 = a.at(0x120);
    f8 = c[0x60] * d110 + c[0x5F] * f8 + 1e-08F;
    a.at(0x114) = (c[0x5D] + 1e-05F) * d110 + c[0x5E] * a.at(0x118) + 1e-08F;
    a.at(0x1D4) = f8;
    a.at(0x8C) = c[0x63] * f8 + c[0x62] * a.at(0x114) + 1e-08F;
    const float f8c = a.at(0x8C);
    a.at(0x10C) = f8c * 1e-05F + d110 * c[0x65] + f8c * c[0x67] + 1e-08F;
    const float f10 = c[0x75] * a.at(0x128) + (c[0x74] + 1e-05F) * d120 + 1e-08F;
    const float f8d = (c[0x69] + 1e-05F) * f8c + c[0x6A] * a.at(0x10C) + a.at(0x90) * 1e-05F
                      + a.at(0x90) * c[0x6C] + a.at(0x94) * 1e-05F + a.at(0x94) * c[0x6E]
                      + a.at(0x98) * 1e-05F + a.at(0x98) * c[0x70] + a.at(0x9C) * 1e-05F
                      + a.at(0x9C) * c[0x72] + 1e-08F;
    a.at(0x94) = f8d;
    a.at(0x124) = f10;
    float f6b = c[0x76] * f8d + d120 * c[0x77] + 1e-08F;
    a.at(0x1D4) = f6b;
    float f9b = c[0x7A] * f6b + c[0x79] * a.at(0x124) + 1e-08F;
    a.at(0x1D4) = f9b;
    a.at(0x9C) = f10;
    a.at(0x11C) = f9b * 1e-05F + d120 * c[0x7C] + f9b * c[0x7E] + 1e-08F;
    f5 = (c[0x83] + 1e-05F) * f10 + a.at(0x108) * 1e-05F + a.at(0x108) * c[0x85] + 1e-08F;
    a.at(0x1D4) = f5;
    a.at(0x1E8) = c[0x88] * f5 + chain_a_tap * c[0x87] + 1e-08F;

    // ---- chain two -------------------------------------------------------------------------
    float y = c[0x8C] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = y;
    y = y * 1e-05F + a.at(0x24) * c[0x8E] + y * c[0x90] + 1e-08F;
    a.at(0x54) = y;
    a.at(0x20) = y * 1e-05F + a.at(0x24) * c[0x92] + y * c[0x94] + 1e-08F;
    t1 = y * c[0x98] + c[0x99] * a.at(0x58) + a.at(0x5C) * c[0x9A] + 1e-08F;
    a.at(0x58) = t1;
    t2 = c[0x9D] * t1 + a.at(0x5C) * c[0x9C] + a.at(0x60) * c[0x9B] + 1e-08F;
    a.at(0x5C) = t2;
    t2 = c[0xA0] * t2 + a.at(0x60) * c[0x9F] + c[0x9E] * a.at(0x64) + 1e-08F;
    a.at(0x60) = t2;
    for (const int k : {0xA3, 0xA6, 0xA9, 0xAC, 0xAF}) {
        t2 = c[k] * t2 + 1e-08F;
        a.at(0x1D4) = t2;
    }
    a.at(0x1EC) = clamp_unit(c[0xB2] * t2);
    shape = c[0xB5] * a.at(0x1EC);
    if (shape <= 0.0F) {
        shape = -shape;
    }
    shape = c[0xB6] * 0.5F + shape;
    a.at(0x1D4) = shape + 1e-08F;
    if (shape < 0.0F) {
        a.at(0x1D4) = c[0xBC] * 1.52588e-05F + 1e-08F;
    }
    f5 = c[0xC0] * a.at(0x1D4) + c[0xC1] * 0.5F + 1e-08F;
    a.at(0x1D4) = f5;
    knee = c[0xC4] * f5 + c[0xC3] * 0.5F + 1e-08F;
    y = (a.at(0x1EC) * 1e-05F - a.at(0x1EC) * knee) + 1e-08F;
    a.at(0x1D4) = y;
    a.at(0x68) = c[0xCC] * y + 1e-08F;
    t2 = c[0xCE] * a.at(0x6C) + a.at(0x70) * c[0xCD] + c[0xCF] * a.at(0x68) + 1e-08F;
    a.at(0x6C) = t2;
    y = c[0xD0] * a.at(0x74) + a.at(0x70) * c[0xD1] + c[0xD2] * t2 + c[0xD3] * a.at(0x78)
        + c[0xD4] * a.at(0x7C) + 1e-08F;
    a.at(0x74) = y;
    const float d1bc = a.at(0x1BC);
    a.at(0x12C) = c[0xD7] * y + 1e-08F;
    const float chain_b_tap = a.at(0x12C);
    f8 = (c[0xD9] + 1e-05F) * a.at(0x130) + chain_b_tap * 1e-05F + chain_b_tap * c[0xDB]
         + a.at(0x134) * 1e-05F + a.at(0x134) * c[0xDD] + 1e-08F;
    a.at(0x130) = f8;
    const float d1cc = a.at(0x1CC);
    f8 = c[0xE1] * f8 + d1bc * c[0xE2] + 1e-08F;
    a.at(0x1C0) = (c[0xDF] + 1e-05F) * d1bc + c[0xE0] * a.at(0x1C4) + 1e-08F;
    a.at(0x1D4) = f8;
    a.at(0x138) = c[0xE5] * f8 + c[0xE4] * a.at(0x1C0) + 1e-08F;
    const float f8e = a.at(0x138);
    a.at(0x1B8) = f8e * 1e-05F + d1bc * c[0xE7] + f8e * c[0xE9] + 1e-08F;
    const float f10b = c[0xF7] * a.at(0x1F4) + (c[0xF6] + 1e-05F) * d1cc + 1e-08F;
    const float f8f = c[0xEC] * a.at(0x1B8) + (c[0xEB] + 1e-05F) * f8e + a.at(0x13C) * 1e-05F
                      + a.at(0x13C) * c[0xEE] + a.at(0x140) * 1e-05F + a.at(0x140) * c[0xF0]
                      + a.at(0x144) * 1e-05F + a.at(0x144) * c[0xF2] + a.at(0x148) * 1e-05F
                      + a.at(0x148) * c[0xF4] + 1e-08F;
    a.at(0x140) = f8f;
    a.at(0x1F0) = f10b;
    f6b = c[0xF8] * f8f + d1cc * c[0xF9] + 1e-08F;
    a.at(0x1D4) = f6b;
    f9b = c[0xFC] * f6b + c[0xFB] * a.at(0x1F0) + 1e-08F;
    a.at(0x1D4) = f9b;
    a.at(0x148) = f10b;
    a.at(0x1C8) = f9b * 1e-05F + d1cc * c[0xFE] + f9b * c[0x100] + 1e-08F;
    f5 = (c[0x105] + 1e-05F) * f10b + a.at(0x1B4) * 1e-05F + a.at(0x1B4) * c[0x107] + 1e-08F;
    a.at(0x1D4) = f5;
    const float chain_b_out = c[0x10A] * f5 + chain_b_tap * c[0x109] + 1e-08F;
    a.at(0x1EC) = chain_b_out;

    // ---- mix the two chains into the output pair ---------------------------------------------
    const float mix_a = c[0x10E] * a.at(0x1E8) + 1e-08F;
    const float mix_b = c[0x10F] * chain_b_out + 1e-08F;
    a.at(0x1D4) = mix_a;
    a.at(0x1D8) = mix_b;
    a.at(0x1E4) = mix_b * c[0x112] + mix_a * c[0x111] + 1e-08F;
    a.at(0x1DC) = mix_a * c[0x116] + mix_b * c[0x117] + 1e-08F;

    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// The ±2 fold into [-1, 1) with **no** nudge, for the algorithms that inline it.
///
/// `dsp_phase_wrap` adds 1e-08 before folding, and an algorithm that calls it therefore gets the
/// nudge for free. One that has the fold inlined instead carries the nudge in its own constants --
/// Space D asks for 1.48828e-08 where the Hexa Chorus asks for 4.8828e-09, 5.0099998e-06 where it
/// asks for 5e-06, and 1.001e-05 where it asks for 1e-05, each of which is the other plus exactly
/// 1e-08. Using the nudging version there adds it twice, which is worth about 1e-08 of output and
/// is exactly what an impulse comparison catches.
[[nodiscard]] inline float phase_fold(float x) noexcept
{
    while (x < -1.0F) {
        x += 2.0F;
    }
    while (x >= 1.0F) {
        x -= 2.0F;
    }
    return x;
}

/// `dsp_phase_wrap` @ 0x180005C20: fold a float into [-1, 1) by repeated ±2, after the same
/// 1e-08 nudge every node in this engine applies.
[[nodiscard]] inline float phase_wrap(float x) noexcept
{
    x += 1e-08F;
    while (x < -1.0F) {
        x += 2.0F;
    }
    while (x >= 1.0F) {
        x -= 2.0F;
    }
    return x;
}

/// `dsp_wavetable_lookup` @ 0x180005C80: read the delay buffer as a table at a fractional
/// position. The phase is scaled by 2^27 into an int, and the index and fraction come out of that
/// by shifting rather than by float arithmetic — which is why a negative phase clamps to zero
/// rather than wrapping, and why the fraction is a tenth of a bit's worth of resolution.
struct TableTap {
    float fraction;
    float sample;
    float next;
};

[[nodiscard]] inline TableTap wavetable_tap(float phase, Tape& table) noexcept
{
    std::int32_t scaled = 0;
    if (phase >= 0.0F) {
        if (phase >= 16.0F) {
            scaled = 0x7FFFFFFF;
        } else {
            scaled = static_cast<std::int32_t>(phase * 1.3421773e+08F);
        }
    } else {
        scaled = 0;
    }
    const std::int32_t shifted = (scaled >> 4) & 0x0FFFFFFF;
    const int index = shifted >> 10;
    return TableTap{
        .fraction = static_cast<float>((scaled >> 4) & 0x3FF) * 0.0009765625F,
        .sample = table.tap(index),
        .next = table.tap(index + 1),
    };
}

/// `fx_algo_rotary` @ 0x1800382F0: the rotating speaker. Verified sample-identical to the module
/// through `scdec efxir`, which resets the shared state buffer so both sides' rotor phases start
/// together — without that the free-running rotors make any comparison meaningless.
/// Horn and drum are written into the delay
/// buffer at two fixed points and read back by a *rotating* tap — a phase accumulator per rotor,
/// wrapped into [-1, 1), turned into a table position, and interpolated. Each rotor also has its
/// own tremolo, taken from the same phase, so a rotor modulates level and delay together, which is
/// what a Leslie does.
void rotary_sample(float in_left,
                   float in_right,
                   float& out_left,
                   float& out_right,
                   Tape& a,
                   Tape& b,
                   const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float f4 = c[0] * a.at(0x1E0) + 1e-08F;
    float f7 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1D4) = f7;
    f4 = f4 * c[3] + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1F8) = c[4] * f7 + 1e-08F;
    out_right = c[6] * f4 + 1e-08F;

    float f5 = a.at(0x1FC) * c[0x0A] + c[0x0B] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f5;
    f4 = c[0x0E] * f5 + 1e-08F;
    a.at(0x1D4) = f4;
    f4 = f4 * 1e-05F + c[0x10] * a.at(0x20) + f4 * c[0x12] + 1e-08F;
    a.at(0x1D4) = f4;
    f7 = c[0x17] * f4 + 1e-08F;
    a.at(0x80) = f7;
    a.at(0x1C) = c[0x14] * a.at(0x20) + f4 * 1e-05F + f4 * c[0x16] + 1e-08F;

    // The horn feed: a five-pole shelf into the buffer's head.
    f5 = (c[0x1B] + 1e-05F) * a.at(0x80) + a.at(0x84) * 1e-05F + a.at(0x84) * c[0x1D]
         + a.at(0x88) * 1e-05F + a.at(0x88) * c[0x1F] + a.at(0x8C) * 1e-05F + a.at(0x8C) * c[0x21]
         + a.at(0x90) * 1e-05F + a.at(0x90) * c[0x23] + 1e-08F;
    a.at(0x1D4) = f5;
    a.at(0x88) = f5;
    b.tap(0) = c[0x26] * f5 + 1e-08F;

    // The drum feed, into the far half of the buffer.
    a.at(0x94) = f7;
    f5 = (c[0x2B] + 1e-05F) * a.at(0x94) + a.at(0x98) * 1e-05F + a.at(0x98) * c[0x2D]
         + a.at(0x9C) * 1e-05F + a.at(0x9C) * c[0x2F] + 1e-08F;
    a.at(0x1D4) = f5;
    a.at(0x98) = f5;
    b.tap(0x4000) = c[0x32] * f5 + 1e-08F;

    // ---- rotor one -----------------------------------------------------------------------------
    float f6 = a.at(0x104) * c[0x38] + c[0x37] * 0.5F + 1e-05F + c[0x3B] * 0.5F + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = (c[0x3F] + 1e-05F) * f6 + 1e-08F;
    a.at(0x1D4) = f6;
    float rate = c[0x42] * f6 + a.at(0x104) * c[0x43] + 1e-08F;
    a.at(0x100) = rate;
    a.at(0x40) = phase_wrap(c[0x48] * a.at(0x44) + 4.8828e-09F + (rate * 0.00195312F));

    float tremolo = c[0x4D] * a.at(0x40);
    if (tremolo <= 0.0F) {
        tremolo = -tremolo;
    }
    f6 = tremolo + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = f6 * f6 + f6 * 1e-05F + 1e-08F;
    a.at(0x1D4) = f6;
    TableTap tap =
        wavetable_tap(phase_wrap((c[0x56] + 1e-05F) * f6 + c[0x58] * 0.5F + c[0x59] * 0.5F), b);
    a.at(0x1EC) = tap.next;
    a.at(0x1E8) = tap.sample;
    float wet = (tap.sample * c[0x6F]) + (tap.fraction * tap.next) - (tap.fraction * tap.sample);
    float pan = c[0x71] * phase_wrap(c[0x6C] * a.at(0x40) + c[0x6D] * 0.5F);
    a.at(0x1D4) = pan;
    if (pan <= 0.0F) {
        pan = -pan;
    }
    wet += 1e-08F;
    a.at(0x1D4) = wet;
    f5 = b.tap(0x7B);
    a.at(0x1E8) = f5;
    float depth = c[0x72] * 0.5F + pan + 1e-08F;
    f6 = c[0x75] * f5 + wet * c[0x74] + 1e-08F;
    a.at(0x1D4) = f6;
    a.at(0xC4) = f6 * depth + f6 * 1e-05F + 1e-08F;

    // ---- rotor two, the same shape on its own phase ---------------------------------------------
    float pan2 = c[0x81] * phase_wrap(c[0x7D] * a.at(0x40) + c[0x7E] * 0.5F);
    a.at(0x1D4) = pan2;
    if (pan2 <= 0.0F) {
        pan2 = -pan2;
    }
    f6 = pan2 + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = f6 * f6 + f6 * 1e-05F + 1e-08F;
    a.at(0x1D4) = f6;
    tap = wavetable_tap(phase_wrap((c[0x8A] + 1e-05F) * f6 + c[0x8C] * 0.5F + c[0x8D] * 0.5F), b);
    a.at(0x1EC) = tap.next;
    a.at(0x1E8) = tap.sample;
    float wet2 = (tap.sample * c[0xA3]) + (tap.fraction * tap.next) - (tap.fraction * tap.sample);
    float pan3 = c[0xA5] * phase_wrap(c[0xA0] * a.at(0x40) + c[0xA1] * 0.5F);
    a.at(0x1D4) = pan3;
    if (pan3 <= 0.0F) {
        pan3 = -pan3;
    }
    wet2 += 1e-08F;
    a.at(0x1D4) = wet2;
    a.at(0x1E8) = b.tap(0x385);
    const float depth2 = c[0xA6] * 0.5F + pan3 + 1e-08F;
    f6 = wet2 * c[0xA8] + c[0xA9] * a.at(0x1E8) + 1e-08F;
    a.at(0x1D4) = f6;
    a.at(0xC8) = f6 * depth2 + f6 * 1e-05F + 1e-08F;

    // ---- the drum rotor ------------------------------------------------------------------------
    f6 = a.at(0x10C) * c[0xB3] + c[0xB2] * 0.5F + 1e-05F + c[0xB6] * 0.5F + 1e-08F;
    a.at(0x1D4) = f6;
    f6 = (c[0xBA] + 1e-05F) * f6 + 1e-08F;
    a.at(0x1D4) = f6;
    const float rate2 = c[0xBD] * f6 + a.at(0x10C) * c[0xBE] + 1e-08F;
    a.at(0x108) = rate2;
    a.at(0x48) = phase_wrap(c[0xC3] * a.at(0x4C) + 4.8828e-09F + (rate2 * 0.00195312F));
    float trem2 = c[0xC8] * a.at(0x48);
    if (trem2 <= 0.0F) {
        trem2 = -trem2;
    }
    a.at(0x1D4) = trem2 + 1e-08F;
    tap = wavetable_tap(
        phase_wrap((c[0xCC] + 1e-05F) * (trem2 + 1e-08F) + c[0xCE] * 0.5F + c[0xCF] * 0.5F), b);
    const float shelf = a.at(0x188);
    a.at(0x1EC) = tap.next;
    a.at(0x1E8) = tap.sample;
    a.at(0xD4) = (tap.sample * c[0xE5]) + (tap.fraction * tap.next) - (tap.fraction * tap.sample)
                 + 1e-08F;
    a.at(0x1E8) = b.tap(0x4148);
    a.at(0xCC) = c[0xEB] * a.at(0x1E8) + 1e-08F;
    a.at(0x1E8) = b.tap(0x44A4);
    f7 = c[0xF1] * a.at(0x1E8) + 1e-08F;
    a.at(0xD0) = f7;

    // ---- the two output shelves ----------------------------------------------------------------
    a.at(0x180) = a.at(0xC4) * c[0xF5] + a.at(0xC8) * c[0xF6] + a.at(0xCC) * c[0xF7]
                  + f7 * c[0xF8] + a.at(0xD4) * c[0xF9] + 1e-08F;
    const float shelf_r = a.at(0x198);
    float f8 = (c[0xFD] + 1e-05F) * a.at(0x180) + a.at(0x184) * 1e-05F + a.at(0x184) * c[0xFF]
               + shelf * 1e-05F + shelf * c[0x101] + 1e-08F;
    a.at(0x184) = f8;
    f6 = shelf * c[0x103] + shelf * 1e-05F + f8 * 1e-05F + f8 * c[0x105] + a.at(0x18C) * 1e-05F
         + a.at(0x18C) * c[0x107] + 1e-08F;
    a.at(0x188) = f6;
    a.at(0x1E4) = c[0x10A] * f6 + 1e-08F;

    a.at(0x190) = a.at(0xC4) * c[0x10E] + a.at(0xC8) * c[0x10F] + a.at(0xCC) * c[0x110]
                  + f7 * c[0x111] + a.at(0xD4) * c[0x112] + 1e-08F;
    f5 = (c[0x116] + 1e-05F) * a.at(0x190) + a.at(0x194) * 1e-05F + a.at(0x194) * c[0x118]
         + shelf_r * 1e-05F + shelf_r * c[0x11A] + 1e-08F;
    a.at(0x194) = f5;
    f6 = (c[0x11C] + 1e-05F) * shelf_r + f5 * 1e-05F + f5 * c[0x11E] + a.at(0x19C) * 1e-05F
         + a.at(0x19C) * c[0x120] + 1e-08F;
    a.at(0x198) = f6;
    a.at(0x1DC) = c[0x123] * f6 + 1e-08F;

    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// `fx_algo_stereo_eq` @ 0x18001B730: the four-band stereo EQ. Two identical chains of shelving
/// and peaking sections, one per channel, and the only transcribed algorithm with no modulation
/// and no delay line at all — it never touches the second buffer.
///
/// Both chains are fed from the *same* conditioned sample (slot 0x1F8, which the input stage just
/// filled from the right input); the left input only reaches the output stage. That is the same
/// shape OD / OD2's second chain has, and it is the engine's, not a transcription slip.
void stereo_eq_sample(float in_left,
                      float in_right,
                      float& out_left,
                      float& out_right,
                      Tape& a,
                      const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float f4 = c[0] * a.at(0x1E0) + 1e-08F;
    float f6 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1D4) = f6;
    f4 = f4 * c[3] + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1F8) = c[4] * f6 + 1e-08F;
    out_right = c[6] * f4 + 1e-08F;

    // One channel's four sections. The two state slots each section reads are captured before any
    // of them is rewritten, which is what makes this a cascade rather than a feedback path.
    const auto chain = [&a, c](int base, int coef, float input, int out_slot) noexcept {
        const float s0 = a.at(base + 0x08);
        const float s1 = a.at(base + 0x0C);
        a.at(base) = input;
        float f = (c[coef + 0x04] + 1e-05F) * a.at(base) + a.at(base + 0x04) * 1e-05F
                  + a.at(base + 0x04) * c[coef + 0x06] + s0 * 1e-05F + s0 * c[coef + 0x08] + 1e-08F;
        a.at(base + 0x04) = f;
        const float s2 = a.at(base + 0x10);
        const float g = (c[coef + 0x0C] + 1e-05F) * f + s0 * 1e-05F + s0 * c[coef + 0x0E]
                        + s1 * 1e-05F + s1 * c[coef + 0x10] + 1e-08F;
        a.at(base + 0x08) = g;
        a.at(base + 0x14) = s2 * c[coef + 0x15] + c[coef + 0x16] * a.at(base + 0x18) + 1e-08F;
        const float s3 = a.at(base + 0x20);
        float h = s1 * c[coef + 0x14] + c[coef + 0x13] * g
                  + (c[coef + 0x18] + c[coef + 0x17]) * s2 + c[coef + 0x19] * a.at(base + 0x14)
                  + 1e-08F;
        a.at(0x1D4) = h;
        h = c[coef + 0x1C] * h + s2 * c[coef + 0x1B] + 1e-08F;
        a.at(base + 0x0C) = h;
        const float k = c[coef + 0x1F] * h + g * c[coef + 0x1E] + 1e-08F;
        a.at(base + 0x18) = k;
        a.at(base + 0x24) = c[coef + 0x25] * a.at(base + 0x28) + s3 * c[coef + 0x24] + 1e-08F;
        float m = (c[coef + 0x26] + c[coef + 0x27]) * s3 + c[coef + 0x23] * a.at(base + 0x1C)
                  + k * c[coef + 0x22] + c[coef + 0x28] * a.at(base + 0x24) + 1e-08F;
        a.at(0x1D4) = m;
        m = c[coef + 0x2B] * m + s3 * c[coef + 0x2A] + 1e-08F;
        a.at(base + 0x1C) = m;
        const float n = c[coef + 0x2E] * m + k * c[coef + 0x2D] + 1e-08F;
        a.at(0x1D4) = n;
        a.at(out_slot) = c[coef + 0x31] * n + 1e-08F;
    };

    chain(0x40, 0x0A, a.at(0x1FC) * c[0x0A] + 1e-08F, 0x1E4);
    chain(0x80, 0x3F, c[0x3F] * a.at(0x1F8) + 1e-08F, 0x1DC);

    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// Dispatch 7 — the Enhancer.
///
/// The same skeleton as the stereo EQ: the shared input staging, one chain per channel, the shared
/// output staging. Only the chain differs, and the two channels are one chain at a coefficient
/// stride of **0x31** and a state stride of 0x40, with the tail pair 0x10 apart — checked term by
/// term across all twenty-two coefficients each channel reads.
///
/// What the chain is: a one-pole into a three-tap comb, three bare gains in series, a second
/// three-tap, and a two-stage tail that mixes the comb's output back against its own history. The
/// `0x1D4` scratch writes are kept because the engine makes them; they are dead stores into the
/// tape's scratch slot and cost nothing to reproduce.
///
/// The two channels differ in the *order* the decompiler prints the three terms of the first sum,
/// and only there. Floating-point addition is not associative, so that is not nothing — but the
/// terms are identical and one order has to be chosen for a shared body, exactly as `stereo_eq`
/// chose one. The right channel's order is used.
void enhancer_sample(float in_left,
                     float in_right,
                     float& out_left,
                     float& out_right,
                     Tape& a,
                     const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float f4 = c[0] * a.at(0x1E0) + 1e-08F;
    float f6 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1D4) = f6;
    f4 = f4 * c[3] + 1e-08F;
    a.at(0x1D4) = f4;
    a.at(0x1F8) = c[4] * f6 + 1e-08F;
    out_right = c[6] * f4 + 1e-08F;

    const auto chain = [&a, c](int base, int tail, int coef, float input, int out_slot) noexcept {
        a.at(0x1D4) = input;

        const float f5 = c[coef + 0x02] * a.at(base + 0x04) + input * 1e-05F
                         + input * c[coef + 0x04] + 1e-08F;
        a.at(base + 0x08) = f5;
        a.at(base) = f5 * 1e-05F + c[coef + 0x06] * a.at(base + 0x04) + f5 * c[coef + 0x08]
                     + 1e-08F;
        a.at(base + 0x10) = a.at(base + 0x0C) * c[coef + 0x0D] + f5 * c[coef + 0x0C]
                            + a.at(base + 0x14) * c[coef + 0x0E] + 1e-08F;

        // Three gains in series, each landing in the scratch slot on the way past.
        float g = c[coef + 0x11] * a.at(base + 0x10) + 1e-08F;
        a.at(0x1D4) = g;
        g = c[coef + 0x14] * g + 1e-08F;
        a.at(0x1D4) = g;
        g = c[coef + 0x17] * g + 1e-08F;
        a.at(0x1D4) = g;
        a.at(base + 0x18) = c[coef + 0x1A] * g + 1e-08F;

        // Read before the tail is rewritten: this stage mixes against the *previous* sample's
        // output, which is what makes it a filter rather than a feed-forward sum.
        const float held = a.at(tail + 0x08);

        a.at(base + 0x20) = c[coef + 0x1B] * a.at(base + 0x24) + c[coef + 0x1C] * a.at(base + 0x1C)
                            + c[coef + 0x1D] * a.at(base + 0x18) + 1e-08F;
        a.at(tail) = c[coef + 0x20] * a.at(base + 0x20) + c[coef + 0x1F] * f5 + 1e-08F;

        float h = (c[coef + 0x22] + 1e-05F) * a.at(tail + 0x04) + a.at(tail) * 1e-05F
                  + a.at(tail) * c[coef + 0x24] + held * 1e-05F + held * c[coef + 0x26] + 1e-08F;
        a.at(tail + 0x04) = h;
        h = (c[coef + 0x28] + 1e-05F) * held + h * 1e-05F + h * c[coef + 0x2A]
            + a.at(tail + 0x0C) * 1e-05F + a.at(tail + 0x0C) * c[coef + 0x2C] + 1e-08F;
        a.at(tail + 0x08) = h;
        a.at(out_slot) = h;
    };

    chain(0x40, 0x180, 0x0A, a.at(0x1FC) * c[0x0A] + 1e-08F, 0x1E4);
    chain(0x80, 0x190, 0x3B, c[0x3B] * a.at(0x1F8) + 1e-08F, 0x1DC);

    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// `fx_algo_hexa_chorus` @ 0x180022B80 — dispatch 12, the six-voice chorus.
///
/// One delay line and one LFO, read six times at six phase offsets, then mixed twice with
/// different weights to make the stereo pair. The voices are literally one body at a coefficient
/// stride of **0x27** and an output slot four bytes apart, and the two output mixes are one body at
/// a stride of **0x22** — both checked across every coefficient each repetition reads rather than
/// extrapolated from the first.
///
/// The first voice is the odd one and not by much: its phase step *is* the shared accumulator, so
/// it reads `a(0x44)` — the previous sample's `a(0x40)` — and scales its offset by 0.00048828
/// where the other five read the finished accumulator and scale by 0.5. Everything after that is
/// identical, down to the abs and the fractional read.
///
/// The read is `wavetable_tap`, the same fractional tap Rotary uses, and the interpolation is
/// written the way the engine writes it: `sample * gain + fraction * next - fraction * sample`
/// rather than a lerp, because those are not the same in float.
void hexa_chorus_sample(float in_left,
                        float in_right,
                        float& out_left,
                        float& out_right,
                        Tape& a,
                        Tape& b,
                        const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float f5 = c[0] * a.at(0x1E0) + 1e-08F;
    const float f11 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f5;
    a.at(0x1D4) = f11;
    f5 = f5 * c[3] + 1e-08F;
    a.at(0x1D4) = f5;
    a.at(0x1F8) = c[4] * f11 + 1e-08F;
    out_right = c[6] * f5 + 1e-08F;

    // What goes into the line: both inputs summed, one gain, then a one-pole whose state is the
    // pair 0x1C/0x20.
    const float sum = c[0x0A] * a.at(0x1FC) + c[0x0B] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = sum;
    float feed = c[0x0E] * sum + 1e-08F;
    a.at(0x1D4) = feed;
    feed = c[0x10] * a.at(0x20) + feed * 1e-05F + feed * c[0x12] + 1e-08F;
    a.at(0x1D8) = feed;
    a.at(0x1C) = c[0x14] * a.at(0x20) + feed * 1e-05F + feed * c[0x16] + 1e-08F;
    b.tap(0) = c[0x17] * feed + 1e-08F;

    // The shared LFO, advanced once and read six times.
    a.at(0x40) = phase_wrap(c[0x1B] * a.at(0x44) + 4.8828e-09F + c[0x1D] * 0.00048828F);

    const auto voice = [&a, &b, c](int base, int slot, float phase) noexcept {
        float depth = c[base + 0x05] * phase;
        if (depth <= 0.0F) {
            depth = -depth;
        }
        a.at(0x1D4) = depth + 1e-08F;
        const TableTap tap = wavetable_tap(
            phase_wrap((c[base + 0x07] + c[base + 0x0C]) * 0.5F
                       + (c[base + 0x09] + 1e-05F) * (depth + 1e-08F) + 1e-05F),
            b);
        a.at(0x1EC) = tap.next;
        a.at(0x1E8) = tap.sample;
        a.at(slot) = tap.sample * c[base + 0x22] + tap.fraction * tap.next
                     - tap.fraction * tap.sample + 1e-08F;
    };

    voice(0x1B, 0x24, a.at(0x40));
    for (int i = 1; i < 6; ++i) {
        const int base = 0x1B + 0x27 * i;
        const float phase = phase_wrap(c[base] * a.at(0x40) + 5e-06F + c[base + 0x02] * 0.5F);
        a.at(0x1D4) = phase;
        voice(base, 0x24 + 4 * i, phase);
    }

    const float v1 = a.at(0x24);
    const float v2 = a.at(0x28);
    const float v3 = a.at(0x2C);
    const float v4 = a.at(0x30);
    const float v5 = a.at(0x34);
    const float v6 = a.at(0x38);

    // One channel: the six voices at their own weights, a gain, then the two-stage tail that also
    // takes a share of the channel's own input.
    const auto mix = [&](int coef, int tail, int feed_slot, int out_slot) noexcept {
        float m = (c[coef] + 1e-05F) * v1 + v2 * 1e-05F + v2 * c[coef + 0x02] + v3 * 1e-05F
                  + v3 * c[coef + 0x04] + v4 * 1e-05F + v4 * c[coef + 0x06] + v5 * 1e-05F
                  + v5 * c[coef + 0x08] + v6 * 1e-05F + v6 * c[coef + 0x0A] + 1e-08F;
        a.at(0x1D4) = m;
        const float g = c[coef + 0x0D] * m + 1e-08F;
        a.at(0x1D4) = g;
        const float held = a.at(tail + 0x08);
        a.at(tail) = c[coef + 0x10] * g + c[coef + 0x0F] * a.at(feed_slot) + 1e-08F;
        float h = (c[coef + 0x12] + 1e-05F) * a.at(tail + 0x04) + a.at(tail) * 1e-05F
                  + a.at(tail) * c[coef + 0x14] + held * 1e-05F + held * c[coef + 0x16] + 1e-08F;
        a.at(tail + 0x04) = h;
        h = (c[coef + 0x18] + 1e-05F) * held + h * 1e-05F + h * c[coef + 0x1A]
            + a.at(tail + 0x0C) * 1e-05F + a.at(tail + 0x0C) * c[coef + 0x1C] + 1e-08F;
        a.at(tail + 0x08) = h;
        a.at(out_slot) = h;
    };

    mix(0x106, 0x180, 0x1FC, 0x1E4);
    mix(0x128, 0x190, 0x1F8, 0x1DC);

    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// `fx_algo_space_d` @ 0x18003A840 — dispatch 14, Space D.
///
/// Two chorus voices, one per half of the delay line, feeding two output chains. The voices share
/// the Hexa Chorus's shape and its **0x27** coefficient stride — the first advancing the phase
/// accumulator, the second offsetting from it — with its own constants: 1.48828e-08 against
/// 4.8828e-09, 5.0099998e-06 against 5e-06, and a second wrap that adds 1.001e-05 rather than
/// 1e-05. Those are transcribed as printed rather than rounded together.
///
/// The two output chains are one shape at a stride of **0x18**, but the decompiler prints their
/// tails differently — `held * 1e-05 + held * c[k]` on one and `(c[k] + 1e-05) * held` on the
/// other. Those are equal in algebra and not in float, so each is written as printed instead of
/// being folded into a shared body.
void space_d_sample(float in_left,
                    float in_right,
                    float& out_left,
                    float& out_right,
                    Tape& a,
                    Tape& b,
                    const float* c) noexcept
{
    a.at(0x1F8) = in_right;
    float f6 = c[0] * a.at(0x1E0) + 1e-08F;
    float f9 = c[1] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f6;
    a.at(0x1D4) = f9;
    f6 = f6 * c[3] + 1e-08F;
    a.at(0x1D4) = f6;
    a.at(0x1F8) = f9 * c[4] + 1e-08F;
    out_right = c[6] * f6 + 1e-08F;

    // One feed per half of the line, each through its own one-pole.
    f9 = c[0x0A] * a.at(0x1FC) + 1e-08F;
    float f10 = c[0x0B] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = f9;
    a.at(0x1D4) = f10;
    const float f11 = (c[0x0E] + 1e-05F) * f9 + c[0x0C] * a.at(0x18) + 1e-08F;
    f10 = (c[0x11] + 1e-05F) * f10 + c[0x0F] * a.at(0x28) + 1e-08F;
    b.tap(0) = f11;
    const float held18 = a.at(0x18);
    a.at(0x1D4) = f11;
    a.at(0x1D4) = f10;
    b.tap(0x4000) = f10;
    a.at(0x24) = (c[0x17] + 1e-05F) * f10 + c[0x19] * a.at(0x28) + 1e-08F;
    a.at(0x14) = (c[0x14] + 1e-05F) * f11 + c[0x15] * held18 + 1e-08F;

    // The accumulator, then the voice that offsets from it. Same body, 0x27 apart.
    a.at(0x54) = phase_fold(a.at(0x58) * c[0x1D] + 1.48828e-08F + c[0x1F] * 0.00048828F);

    const auto voice = [&a, &b, c](int base, float phase) noexcept {
        float depth = c[base + 0x05] * phase;
        if (depth <= 0.0F) {
            depth = -depth;
        }
        a.at(0x1D4) = depth + 1e-08F;
        const TableTap tap = wavetable_tap(
            phase_fold((c[base + 0x09] + 1e-05F) * (depth + 1e-08F)
                       + (c[base + 0x07] + c[base + 0x0C]) * 0.5F + 1.001e-05F),
            b);
        a.at(0x1EC) = tap.next;
        a.at(0x1E8) = tap.sample;
        return (tap.sample * c[base + 0x22] + tap.fraction * tap.next)
               - tap.sample * tap.fraction + 1e-08F;
    };

    a.at(0x40) = voice(0x1D, a.at(0x54));

    const float phase2 = phase_fold(c[0x44] * a.at(0x54) + 5.0099998e-06F + c[0x46] * 0.5F);
    a.at(0x1D4) = phase2;
    const float wet2 = voice(0x44, phase2);
    a.at(0x44) = wet2;

    // The right chain. Its tail reads the held value as two products, which is how the engine
    // spells it.
    float f8 = a.at(0x40) * c[0x6B] + wet2 * c[0x6C] + 1e-08F;
    a.at(0x1D4) = f8;
    float g = c[0x6F] * f8 + 1e-08F;
    a.at(0x1D4) = g;
    a.at(0x180) = c[0x72] * g + c[0x71] * a.at(0x1FC) + 1e-08F;
    float held = a.at(0x188);
    f8 = (c[0x76] + 1e-05F) * a.at(0x180) + (c[0x74] + 1e-05F) * a.at(0x184) + held * 1e-05F
         + held * c[0x78] + 1e-08F;
    a.at(0x184) = f8;
    float tail = c[0x7A] * held + held * 1e-05F + (c[0x7C] + 1e-05F) * f8
                 + (c[0x7E] + 1e-05F) * a.at(0x18C) + 1e-08F;
    a.at(0x188) = tail;
    a.at(0x1E4) = tail;

    // The left chain, 0x18 along, and its tail folds the same term into one product instead.
    f8 = a.at(0x40) * c[0x83] + wet2 * c[0x84] + 1e-08F;
    a.at(0x1D4) = f8;
    g = c[0x87] * f8 + 1e-08F;
    a.at(0x1D4) = g;
    a.at(0x190) = c[0x8A] * g + c[0x89] * a.at(0x1F8) + 1e-08F;
    f8 = (c[0x8E] + 1e-05F) * a.at(0x190) + (c[0x8C] + 1e-05F) * a.at(0x194)
         + (c[0x90] + 1e-05F) * a.at(0x198) + 1e-08F;
    a.at(0x194) = f8;
    tail = (c[0x94] + 1e-05F) * f8 + (c[0x92] + 1e-05F) * a.at(0x198)
           + (c[0x96] + 1e-05F) * a.at(0x19C) + 1e-08F;
    a.at(0x198) = tail;
    a.at(0x1DC) = tail;

    a.at(0x1F8) = in_left;
    float left = c[0x170] * a.at(0x1E4) + 1e-08F;
    const float left_in = c[0x171] * a.at(0x1F8) + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1D4) = left_in;
    left = c[0x173] * left + 1e-08F;
    a.at(0x1D4) = left;
    a.at(0x1F8) = c[0x174] * left_in + 1e-08F;
    out_left = c[0x176] * left + 1e-08F;
}

/// Which transcription serves a dispatch index.
enum class Processor {
    thru, ///< dispatch 0, and dispatch 1's cleared "no effect" state
    overdrive, ///< dispatch 3 and 4 — one dataflow, two presets
    od_od2, ///< dispatch 42 — two overdrive chains in parallel
    rotary, ///< dispatch 9 — the rotating speaker
    stereo_eq, ///< dispatch 2 — the four-band stereo EQ
    enhancer, ///< dispatch 7 — the Enhancer
    hexa_chorus, ///< dispatch 12 — six chorus voices off one delay line
    space_d, ///< dispatch 14 — two voices, one per half of the line
    efx_reverb, ///< dispatch 0x19, with the inverted output routing
    passthrough, ///< not transcribed yet: signal passes unchanged
};

[[nodiscard]] Processor processor_for(int dispatch) noexcept
{
    switch (dispatch) {
    case 0:
    case 1:
        return Processor::thru;
    case 3:
    case 4:
        return Processor::overdrive;
    case 0x19:
        return Processor::efx_reverb;
    case 42:
        return Processor::od_od2;
    case 9:
        return Processor::rotary;
    case 2:
        return Processor::stereo_eq;
    case 7:
        return Processor::enhancer;
    case 12:
        return Processor::hexa_chorus;
    case 14:
        return Processor::space_d;
    default:
        return Processor::passthrough;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------

struct InsertionEffect::Impl {
    // Tables out of the DLL.
    std::vector<EfxRecord> directory;
    std::vector<std::array<std::uint8_t, RegisterFile::count>> width_maps;
    std::array<std::pair<std::uint16_t, std::uint16_t>, dispatch_count> preset_slices{};
    std::vector<std::uint32_t> preset_data;

    // Overdrive/Distortion curves.
    std::array<std::uint8_t, 128> drive_a{};
    std::array<std::uint8_t, 128> drive_b{};
    std::array<std::uint8_t, 128> pan_left{};
    std::array<std::uint8_t, 128> pan_right{};
    std::array<std::uint8_t, 128> level_curve{};
    std::array<std::uint8_t, 2> amp_switch_a{};
    std::array<std::uint8_t, 2> amp_switch_b{};
    std::array<std::uint16_t, 128> low_gain_f{};
    std::array<std::uint16_t, 128> low_gain_d{};
    std::array<std::uint16_t, 128> low_gain_h{};
    std::array<std::uint16_t, 128> high_gain_f{};
    std::array<std::uint16_t, 128> high_gain_d{};
    std::array<std::uint16_t, 128> high_gain_h{};
    std::array<std::uint16_t, amp_bank_register_count> amp_bank_registers{};

    // OD / OD2.
    std::array<std::array<std::uint8_t, 2>, 19> od2_bank{};
    std::array<std::uint8_t, 128> od2_drive_alt{};
    std::array<std::uint16_t, amp_bank_register_count> od2_amp_bank_a{};
    std::array<std::uint16_t, amp_bank_register_count> od2_amp_bank_b{};
    std::array<std::uint16_t, 128> eq_low_alt_f{};
    std::array<std::uint16_t, 128> eq_low_alt_d{};
    std::array<std::uint16_t, 128> eq_low_alt_h{};
    std::array<std::uint16_t, 128> eq_high_alt_f{};
    std::array<std::uint16_t, 128> eq_high_alt_d{};
    std::array<std::uint16_t, 128> eq_high_alt_h{};
    std::array<std::uint8_t, 2> eq_freq_latch{};
    std::array<std::uint8_t, 5> eq_q_latch{};
    std::array<std::vector<std::uint8_t>, 17> bank_tables;
    std::array<std::uint8_t, 64> bank_pairs{};
    std::array<std::array<std::uint16_t, bank_register_count>, 4> eq_mid_registers{};

    std::array<std::uint16_t, 128> rotary_rate{};
    std::array<std::uint8_t, 128> rotary_spread{};
    std::array<std::uint16_t, 128> rotary_speed{};

    std::array<std::uint8_t, 2> od2_type_latch{};
    std::array<std::uint8_t, 2> od2_amp_type_latch{};
    std::array<std::uint8_t, 2> od2_amp_switch_latch{};
    std::array<std::array<std::uint16_t, 4>, 16> amp_bank_values{};

    // EFX Reverb tables.
    std::array<std::uint16_t, 2> rev_regs16{};
    std::array<std::uint16_t, 17> rev_regs8{};
    std::array<std::uint16_t, 19> rev_defaults{};
    std::array<std::uint16_t, 32> rev_default_taps{};
    std::array<std::array<std::uint16_t, 32>, 6> rev_char_taps{};
    std::array<std::array<std::uint16_t, 19>, 6> rev_char_values{};
    std::array<std::uint16_t, 32> rev_fallback_taps{};
    std::array<std::uint16_t, 19> rev_fallback_values{};
    std::array<std::uint16_t, 128> rev_lpf_curve{};
    std::array<std::uint16_t, 128> rev_time_curve{};
    std::array<std::uint16_t, 128> rev_feedback_a{};
    std::array<std::uint16_t, 128> rev_feedback_b{};
    std::array<std::uint8_t, 128> rev_p16_a{};
    std::array<std::uint8_t, 128> rev_p16_b{};

    /// The live tap program (`DAT_181a0f108`…), float indices into buffer B.
    std::array<std::int32_t, reverb_tap_count> reverb_taps{};

    RegisterFile registers;
    Tape tape_a{1 << 15};
    Tape tape_b{1 << 17};

    std::array<std::uint8_t, parameter_bytes> params{};

    /// The persistent change-detection shadow — the engine's latch bytes. It survives type
    /// selects: a parameter whose new type's default equals the latched value does **not**
    /// re-apply, which is why a fresh selection's registers are the preset fill and not the
    /// curves (proven by poking `40 03 13` in the live engine and watching the pair register).
    std::array<std::uint8_t, parameter_bytes> shadow{};

    int record_index = 0;
    int dispatch = 0;
    Processor processor = Processor::thru;

    // The apply handlers' clamped-mode latches (`DAT_181a1dea0/dea1/dea2/deb8`). The slots are
    // shared between effects, exactly as the engine's globals are: Overdrive's drive byte lands
    // in the slot the reverb reads as its character latch, and the reverb's time byte lands in
    // the slot Overdrive reads as its amp-simulator switch. Faithful, if surprising.
    std::uint8_t latch_dea0 = 0;
    std::uint8_t amp_type_latch = 0;
    std::uint8_t amp_switch_latch = 0;
    std::uint8_t output_latch = 1;

    /// The `40 03 17`–`19` common sends, as the engine stores them: byte << 7 into a Q15 gain.
    std::uint16_t send_reverb = 0;
    std::uint16_t send_chorus = 0;
    std::uint16_t send_delay = 0;

    /// The internal output toggle behind params[0x1A]; net unity here, pending an A/B calibration
    /// of the engine's ramp target (2.0) against its send-matrix input tap.
    bool output_enabled = true;

    explicit Impl(const RomImage& rom) { load_tables(rom); }

    void load_tables(const RomImage& rom);
    void select_key(std::uint16_t key);
    void select_algorithm(int index);
    void apply(bool on_select);
    void apply_common_tail();
    void apply_overdrive();
    void amp_bank_program(const std::array<std::uint16_t, amp_bank_register_count>& bank,
                          std::uint8_t type,
                          std::uint8_t simulator);
    void apply_od_od2();
    void apply_rotary(bool on_select);
    void apply_stereo_eq();
    void bank_load(const std::array<std::uint16_t, bank_register_count>& registers_,
                   std::uint8_t frequency,
                   std::uint8_t q,
                   std::uint8_t gain);
    void od2_chain(int chain, int type_param, int drive_param, int control_offset);
    void apply_efx_reverb();
    void reverb_character_program();

    [[nodiscard]] bool changed(int index) const noexcept
    {
        return params[static_cast<std::size_t>(index)] != shadow[static_cast<std::size_t>(index)];
    }
};

void InsertionEffect::Impl::load_tables(const RomImage& rom)
{
    const auto read_u16s = [&rom](std::int64_t offset, std::span<std::uint16_t> out) {
        std::vector<std::uint8_t> raw = rom.read(offset, out.size() * 2);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::uint16_t>(raw[i * 2] | (raw[i * 2 + 1] << 8));
        }
    };
    const auto read_u8s = [&rom](std::int64_t offset, std::span<std::uint8_t> out) {
        std::vector<std::uint8_t> raw = rom.read(offset, out.size());
        std::copy(raw.begin(), raw.end(), out.begin());
    };

    // The directory.
    {
        const std::vector<std::uint8_t> raw =
            rom.read(directory_offset, directory_records * directory_stride);
        directory.reserve(directory_records);
        for (int i = 0; i < directory_records; ++i) {
            const std::uint8_t* rec = raw.data() + static_cast<std::size_t>(i) * directory_stride;
            EfxRecord record;
            record.name.assign(reinterpret_cast<const char*>(rec), 12);
            while (!record.name.empty() && record.name.back() == ' ') {
                record.name.pop_back();
            }
            record.type_key = static_cast<std::uint16_t>(rec[0x0C] | (rec[0x0D] << 8));
            record.dispatch = static_cast<std::uint16_t>(rec[0x0E] | (rec[0x0F] << 8));
            // The defaults handler is `movups` from a static block whose +0x0C holds the bytes;
            // rather than follow code, the block's address is recovered the same way the dump
            // tool does — but here the defaults were verified to sit at the handler's source
            // block, so they are read via the per-record accessor table below.
            directory.push_back(std::move(record));
        }
    }

    // Defaults. Each record's `param_defaults` handler copies a static 0x1C-byte block; the block
    // addresses are recovered from the handler code by the dump tool. Reading code out of `.text`
    // needs its own section adjustment (-0xC00), and following two instructions is all it takes:
    // `movups xmm0, [rip+src]` at the entry names the block (Thru's plain `lea` names it
    // directly).
    {
        const std::vector<std::uint8_t> raw =
            rom.read(directory_offset, directory_records * directory_stride);
        constexpr std::int64_t text_adjust = 0xC00;
        for (int i = 0; i < directory_records; ++i) {
            const std::uint8_t* rec = raw.data() + static_cast<std::size_t>(i) * directory_stride;
            std::int64_t handler = 0;
            std::memcpy(&handler, rec + 0x18, 8);
            const std::vector<std::uint8_t> code =
                rom.read(handler - image_base - text_adjust, 16);
            std::int64_t block = 0;
            std::int32_t displacement = 0;
            if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05) {
                std::memcpy(&displacement, code.data() + 3, 4);
                block = handler + 7 + displacement;
            } else if (code[0] == 0x0F && code[1] == 0x10 && code[2] == 0x05) {
                std::memcpy(&displacement, code.data() + 3, 4);
                block = handler + 7 + displacement;
            } else {
                throw std::runtime_error(
                    "An EFX defaults handler does not look like this build's.");
            }
            const std::vector<std::uint8_t> defaults = rom.read(file_offset(block) + 0x0C, 0x1C);
            std::copy(defaults.begin(), defaults.end(), directory[static_cast<std::size_t>(i)]
                                                            .defaults.begin());
        }
    }

    // Width maps: 67 pointers, each to a 0x180-byte map in `.data` — read what the file backs,
    // zero what BSS would have zeroed.
    {
        std::vector<std::uint8_t> raw = rom.read(width_map_pointers_offset, dispatch_count * 8);
        width_maps.resize(dispatch_count);
        for (int i = 0; i < dispatch_count; ++i) {
            std::int64_t pointer = 0;
            std::memcpy(&pointer, raw.data() + static_cast<std::size_t>(i) * 8, 8);
            auto& map = width_maps[static_cast<std::size_t>(i)];
            map.fill(0);
            const std::int64_t from = pointer - data_virtual;
            if (from < 0) {
                throw std::runtime_error("An EFX width map is outside this build's .data.");
            }
            const std::int64_t backed =
                std::clamp<std::int64_t>(data_raw_size - from, 0, RegisterFile::count);
            if (backed > 0) {
                const std::vector<std::uint8_t> bytes =
                    rom.read(data_raw + from, static_cast<std::size_t>(backed));
                std::copy(bytes.begin(), bytes.end(), map.begin());
            }
        }
    }

    // Preset slices and the packed data they index.
    {
        std::vector<std::uint8_t> raw = rom.read(preset_slices_offset, dispatch_count * 4);
        std::size_t end = 0;
        for (int i = 0; i < dispatch_count; ++i) {
            const auto offset = static_cast<std::uint16_t>(
                raw[static_cast<std::size_t>(i) * 4] | (raw[static_cast<std::size_t>(i) * 4 + 1] << 8));
            const auto length = static_cast<std::uint16_t>(
                raw[static_cast<std::size_t>(i) * 4 + 2]
                | (raw[static_cast<std::size_t>(i) * 4 + 3] << 8));
            preset_slices[static_cast<std::size_t>(i)] = {offset, length};
            end = std::max(end, static_cast<std::size_t>(offset) + length);
        }
        const std::vector<std::uint8_t> data = rom.read(preset_data_offset, end * 4);
        preset_data.resize(end);
        std::memcpy(preset_data.data(), data.data(), end * 4);
    }

    // Overdrive/Distortion curves.
    read_u8s(drive_curve_a_offset, drive_a);
    read_u8s(drive_curve_b_offset, drive_b);
    read_u8s(pan_left_offset, pan_left);
    read_u8s(pan_right_offset, pan_right);
    read_u8s(level_curve_offset, level_curve);
    read_u8s(amp_switch_a_offset, amp_switch_a);
    read_u8s(amp_switch_b_offset, amp_switch_b);
    read_u16s(low_gain_f_offset, low_gain_f);
    read_u16s(low_gain_d_offset, low_gain_d);
    read_u16s(low_gain_h_offset, low_gain_h);
    read_u16s(high_gain_f_offset, high_gain_f);
    read_u16s(high_gain_d_offset, high_gain_d);
    read_u16s(high_gain_h_offset, high_gain_h);
    read_u16s(amp_bank_registers_offset, amp_bank_registers);
    {
        // The sixteen amp-bank value tables, in the order the routine reads them.
        constexpr std::array<std::int64_t, 16> tables{
            0x181989E38, 0x181988CB8, 0x181988018, 0x181987D98, 0x181988F38, 0x181989438,
            0x181989938, 0x181989BB8, 0x181988298, 0x1819896B8, 0x1819891B8, 0x18198A0B8,
            0x181988518, 0x181988A38, 0x181986858, 0x181897364,
        };
        for (std::size_t i = 0; i < tables.size(); ++i) {
            read_u16s(file_offset(tables[i]), amp_bank_values[i]);
        }
    }

    // OD / OD2: the nineteen two-entry bank tables, the alternate drive curve, and the two
    // amp-simulator register banks.
    for (std::size_t i = 0; i < od2_bank_tables.size(); ++i) {
        read_u8s(file_offset(od2_bank_tables[i]), od2_bank[i]);
    }
    read_u8s(od2_drive_alt_offset, od2_drive_alt);
    for (std::size_t i = 0; i < bank_coef_tables.size(); ++i) {
        bank_tables[i] = rom.read(file_offset(bank_coef_tables[i]),
                                  static_cast<std::size_t>(bank_table_rows) * bank_row_bytes);
    }
    read_u8s(bank_pair_offset, bank_pairs);
    for (std::size_t i = 0; i < eq_mid_banks.size(); ++i) {
        read_u16s(file_offset(eq_mid_banks[i]), eq_mid_registers[i]);
    }
    read_u16s(eq_low_alt_f_offset, eq_low_alt_f);
    read_u16s(eq_low_alt_d_offset, eq_low_alt_d);
    read_u16s(eq_low_alt_h_offset, eq_low_alt_h);
    read_u16s(eq_high_alt_f_offset, eq_high_alt_f);
    read_u16s(eq_high_alt_d_offset, eq_high_alt_d);
    read_u16s(eq_high_alt_h_offset, eq_high_alt_h);
    read_u16s(rotary_rate_offset, rotary_rate);
    read_u8s(rotary_spread_offset, rotary_spread);
    read_u16s(rotary_speed_offset, rotary_speed);
    read_u16s(od2_amp_bank_a_offset, od2_amp_bank_a);
    read_u16s(od2_amp_bank_b_offset, od2_amp_bank_b);

    // EFX Reverb: register bank, defaults, the six character tables reached through their two
    // pointer arrays, and the parameter curves.
    read_u16s(reverb_bank_regs16_offset, rev_regs16);
    read_u16s(reverb_bank_regs8_offset, rev_regs8);
    read_u16s(reverb_bank_defaults_offset, rev_defaults);
    read_u16s(reverb_default_taps_offset, rev_default_taps);
    read_u16s(reverb_fallback_taps_offset, rev_fallback_taps);
    read_u16s(reverb_fallback_values_offset, rev_fallback_values);
    {
        const std::vector<std::uint8_t> tap_pointers = rom.read(reverb_char_tap_pointers, 6 * 8);
        const std::vector<std::uint8_t> value_pointers =
            rom.read(reverb_char_value_pointers, 6 * 8);
        for (std::size_t i = 0; i < 6; ++i) {
            std::int64_t pointer = 0;
            std::memcpy(&pointer, tap_pointers.data() + i * 8, 8);
            read_u16s(file_offset(pointer), rev_char_taps[i]);
            std::memcpy(&pointer, value_pointers.data() + i * 8, 8);
            read_u16s(file_offset(pointer), rev_char_values[i]);
        }
    }
    read_u16s(reverb_lpf_curve_offset, rev_lpf_curve);
    read_u16s(reverb_time_curve_offset, rev_time_curve);
    read_u16s(reverb_feedback_a_offset, rev_feedback_a);
    read_u16s(reverb_feedback_b_offset, rev_feedback_b);
    read_u8s(reverb_p16_a_offset, rev_p16_a);
    read_u8s(reverb_p16_b_offset, rev_p16_b);
}

void InsertionEffect::Impl::select_key(std::uint16_t key)
{
    // The engine scans 0x42 records and leaves the dispatch at 0 (Thru) when nothing matches —
    // an unknown key is a working Thru, not silence.
    int found = -1;
    for (std::size_t i = 0; i < directory.size(); ++i) {
        if (directory[i].type_key == key) {
            found = static_cast<int>(i);
            break;
        }
    }
    record_index = found >= 0 ? found : 0;
    const EfxRecord& record = directory[static_cast<std::size_t>(record_index)];

    // `fx_get_default_params_for_type`: the type's defaults become the live block. The control
    // bytes past 0x1B are SysEx state of their own and survive a type change.
    std::copy(record.defaults.begin(), record.defaults.end(), params.begin());

    // The select poisons these shadow bytes to 0xFF, so the level, the three sends and the
    // routing toggle always re-fire; everything else compares against the persistent latches.
    shadow[0x13] = 0xFF;
    shadow[0x14] = 0xFF;
    shadow[0x15] = 0xFF;
    shadow[0x16] = 0xFF;
    shadow[0x1A] = 0xFF;

    select_algorithm(found >= 0 ? record.dispatch : 0);
    apply(true);
}

void InsertionEffect::Impl::select_algorithm(int index)
{
    dispatch = index == 1 ? 0 : index;
    processor = processor_for(index);
    registers.set_width_map(width_maps[static_cast<std::size_t>(dispatch)].data());
    if (index == 1) {
        registers.clear_coefficients();
    }
    const auto [offset, length] = preset_slices[static_cast<std::size_t>(index)];
    registers.fill_preset(
        std::span<const std::uint32_t>{preset_data}.subspan(offset, length));

    // `fx_output_routing_set` @ 0x180061E60 runs on every selection: the block's two routing
    // pairs sit at registers 0x83/0x84 and 0x1F3/0x1F4, normally 0x7F/0x20, and the EFX Reverb
    // (dispatch 0x19) and Lo-Fi 2 (0x41) swap them.
    const bool inverted = index == 0x19 || index == 0x41;
    registers.write_slew(0x84, inverted ? 0x7F : 0x20);
    registers.write_slew(0x1F4, inverted ? 0x7F : 0x20);
    registers.write_slew(0x83, inverted ? 0x20 : 0x7F);
    registers.write_slew(0x1F3, inverted ? 0x20 : 0x7F);

    // `fx_transition_commit_params` restores the block's wet levels after every switch.
    registers.write_slew(0x86, 0x7F);
    registers.write_slew(0x1F6, 0x7F);
    registers.write_slew(0x81, 0x7F);
    registers.write_slew(0x1F1, 0x7F);
}

void InsertionEffect::Impl::apply(bool on_select)
{
    switch (processor) {
    case Processor::overdrive:
        apply_overdrive();
        break;
    case Processor::od_od2:
        apply_od_od2();
        break;
    case Processor::rotary:
        apply_rotary(on_select);
        break;
    case Processor::stereo_eq:
        apply_stereo_eq();
        break;
    case Processor::enhancer:
    case Processor::space_d:
    case Processor::hexa_chorus:
        // Only the level, and that is measured rather than assumed: with the preset fill in place
        // the whole 384-register file matches the module except registers 0x80 and 0x1F0, which
        // the type's own handler writes from its level byte through the shared curve. Hexa Chorus
        // defaults to `0x70`, and `level_curve[112]` is 107 — exactly what the live block reads.
        //
        // **The Enhancer was left out of this and the register diff could not see it**, because its
        // default level is `0x7F` and `level_curve[127]` is 127 — the same value the untranscribed
        // fallback hard-codes. It agreed at the default and nowhere else: at level `0x40` the module
        // programs 53/128 where this wrote 127/128. A comparison taken only at a type's defaults
        // cannot tell a handler that computes the right answer from one that is a constant.
        //
        // The other twenty GS parameters are **not** mapped: this type's `40 03 03`–`16` writes do
        // not reach its registers, so its rate, depth, pre-delay and balance stay at the preset.
        // A file that selects Hexa Chorus and leaves it alone is right; one that adjusts it gets
        // the default voicing. Transcribing `fx_apply_hexa_chorus` @ `0x18004B980` is what closes
        // that, and the register diff is how it would be checked.
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
        apply_common_tail();
        break;
    case Processor::efx_reverb:
        apply_efx_reverb();
        break;
    case Processor::thru:
    case Processor::passthrough:
        // Thru's handler (`fx_param_apply_5aab0`) sets its two level registers on the select
        // commit only, and runs the common tail either way; types without a transcription still
        // get the tail, so their sends and routing stay live.
        if (on_select) {
            registers.write_slew(0x80, 0x7F);
            registers.write_slew(0x1F0, 0x7F);
        }
        apply_common_tail();
        break;
    }
}

/// The send and routing bytes every apply handler ends with. Only fired branches update the
/// shadow — that is the engine's latch behaviour, and it is what keeps a later type select from
/// re-applying parameters whose defaults match the latched state.
void InsertionEffect::Impl::apply_common_tail()
{
    send_reverb = static_cast<std::uint16_t>(params[0x14] << 7);
    send_chorus = static_cast<std::uint16_t>(params[0x15] << 7);
    send_delay = static_cast<std::uint16_t>(params[0x16] << 7);
    shadow[0x14] = params[0x14];
    shadow[0x15] = params[0x15];
    shadow[0x16] = params[0x16];
    if (params[0x1A] < 2) {
        output_latch = params[0x1A];
    } else {
        params[0x1A] = output_latch;
    }
    output_enabled = output_latch != 0;
    shadow[0x1A] = output_latch;
}

/// `fx_param_apply_53a10`, the Overdrive/Distortion handler.
void InsertionEffect::Impl::apply_overdrive()
{
    apply_common_tail();

    // Tone shelves, 16-bit filter coefficients from the gain curves.
    if (changed(0x10)) {
        const std::size_t v = params[0x10];
        registers.write16(0x10F, low_gain_f[v]);
        registers.write16(0x10D, low_gain_d[v]);
        registers.write16(0x111, low_gain_h[v]);
        shadow[0x10] = params[0x10];
    }
    if (changed(0x11)) {
        registers.write_slew(0x80, 0);
        registers.write_slew(0x1F0, 0);
        const std::size_t v = params[0x11];
        registers.write16(0x115, high_gain_f[v]);
        registers.write16(0x113, high_gain_d[v]);
        registers.write16(0x117, high_gain_h[v]);
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
        shadow[0x11] = params[0x11];
    }
    if (changed(0x13)) {
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
        shadow[0x13] = params[0x13];
    }

    // Amp simulator switch (0/1, latched) and amp type (0–3, latched).
    if (params[2] < 2) {
        amp_switch_latch = params[2];
    } else {
        params[2] = amp_switch_latch;
    }
    if (changed(2)) {
        registers.write_slew(0x10C, amp_switch_a[amp_switch_latch]);
        registers.write_slew(0x10B, amp_switch_b[amp_switch_latch]);
        shadow[2] = amp_switch_latch;
    }
    if (params[1] < 4) {
        amp_type_latch = params[1];
    } else {
        params[1] = amp_type_latch;
    }
    if (changed(1)) {
        amp_bank_program(amp_bank_registers, amp_type_latch, amp_switch_latch);
        shadow[1] = amp_type_latch;
    }

    // Drive and pan, always refreshed — in the engine the EFX control offsets ride in here, which
    // is why these two run even in the control-only mode. Control sources are not modulated yet,
    // so the offsets are zero.
    const auto drive = static_cast<std::size_t>(std::clamp<int>(params[0], 0, 0x7F));
    registers.write_pair(0xA5, drive_a[drive], 0xD9, drive_b[drive]);
    latch_dea0 = params[0];
    shadow[0] = params[0];
    const auto pan = static_cast<std::size_t>(std::clamp<int>(params[0x12], 0, 0x7F));
    registers.write_slew(0x11B, pan_left[pan]);
    registers.write_slew(0x11C, pan_right[pan]);
    shadow[0x12] = params[0x12];
}

/// `chorus_load_algo_regs` @ 0x180005860, programming the amp-simulator register bank.
void InsertionEffect::Impl::amp_bank_program(
    const std::array<std::uint16_t, amp_bank_register_count>& bank,
    std::uint8_t type,
    std::uint8_t simulator)
{
    if (type >= 4) {
        return;
    }
    const std::uint8_t sw = std::min<std::uint8_t>(simulator, 1);
    const auto& r = bank;
    const auto value = [this, type](int table) {
        return amp_bank_values[static_cast<std::size_t>(table)][type];
    };
    const auto low_byte = [this, type](int table) {
        return static_cast<std::uint8_t>(
            amp_bank_values[static_cast<std::size_t>(table)][type] & 0xFF);
    };

    registers.write_slew(r[0], 0);
    registers.write16(r[0x11], value(0));
    registers.write16(r[0x10], value(1));
    registers.write16(r[0xF], value(2));
    registers.write16(r[0xE], value(2));
    registers.write_slew(r[0xD], low_byte(3));
    registers.write_slew(r[0x12], low_byte(4));
    registers.write16(r[0xC], value(5));
    registers.write16(r[0xB], value(6));
    registers.write16(r[10], value(7));
    registers.write16(r[9], value(8));
    registers.write16(r[8], value(9));
    registers.write16(r[7], value(10));
    registers.write16(r[6], value(10));
    registers.write_slew(r[5], low_byte(11));
    registers.write16(r[4], value(12));
    registers.write16(r[3], value(13));
    registers.write16(r[2], value(14));
    registers.write_slew(r[0], amp_switch_a[sw]);
    registers.write_slew(r[1], amp_switch_b[sw]);
}

/// `reverb_load_algo_regs` @ 0x1800053E0. Frequency selects a coefficient table and a byte pair;
/// gain and Q select a five-byte row inside it; the row's fifth byte holds four flags, one per
/// register group, that set the wide-scale bit. Each of the first four bytes is written twice —
/// once merging the value alone, once with its flag — which is the engine's own order and matters
/// because the second write reads the mirror the first one left.
void InsertionEffect::Impl::bank_load(
    const std::array<std::uint16_t, bank_register_count>& bank,
    std::uint8_t frequency,
    std::uint8_t q,
    std::uint8_t gain)
{
    registers.write_slew(bank[6], 0);

    const int clamped_q =
        static_cast<std::int8_t>(q) < 0 ? 0 : std::min<int>(static_cast<std::int8_t>(q), 4);
    // Gain is a window, not a scale: everything below 0x34 is the bottom of the table, 0x34-0x4C
    // maps straight through, and everything above pins to the top row.
    const int mapped_gain = gain < 0x34 ? 0 : (gain < 0x4D ? gain - 0x34 : 0x18);

    const std::size_t selector = static_cast<std::size_t>(frequency >> 3);
    const std::vector<std::uint8_t>& table =
        bank_tables[selector < bank_tables.size() ? selector : bank_default_table];
    const std::size_t pair = selector * 2;
    const std::uint8_t pair_value = bank_pairs[pair];
    const bool pair_flag = (bank_pairs[pair + 1] >> 7) != 0;

    const std::size_t row =
        static_cast<std::size_t>(((mapped_gain * 5) + clamped_q) * bank_row_bytes);
    if (row + 4 >= table.size()) {
        return;
    }
    const std::uint8_t flags = table[row + 4];
    const auto flag = [flags](int k) { return ((flags >> (7 - k)) & 1) != 0; };

    registers.write_slew(bank[2], pair_value);
    registers.write_flagged(bank[2], pair_value, pair_flag);

    registers.write_slew(bank[0], table[row]);
    registers.write_slew(bank[1], table[row]);
    registers.write_flagged(bank[0], table[row], flag(0));
    registers.write_flagged(bank[1], table[row], flag(0));

    registers.write_slew(bank[3], table[row + 1]);
    registers.write_slew(bank[4], table[row + 1]);
    registers.write_flagged(bank[3], table[row + 1], flag(1));
    registers.write_flagged(bank[4], table[row + 1], flag(1));

    registers.write_slew(bank[5], table[row + 2]);
    registers.write_flagged(bank[5], table[row + 2], flag(2));

    registers.write_flagged(bank[6], 0, flag(3));
    registers.write_slew(bank[6], table[row + 3]);
    registers.write_flagged(bank[6], table[row + 3], flag(3));
}

/// `fx_param_apply_48680`, the stereo EQ handler — the two shelving bands and the level.
///
/// **The two mid bands are not applied here.** They are programmed by a four-argument bank loader
/// (`reverb_load_algo_regs`) that unpacks a per-band coefficient table by frequency and Q, and it
/// is not transcribed yet. Editing `40 03 07`-`40 03 0C` therefore does nothing in this engine,
/// where the shelves and the level respond. The block's *default* state is unaffected — the
/// per-type preset fill programs all 384 registers correctly on selection, verified against the
/// live engine — so a file that selects the EQ and leaves it alone renders exactly right.
void InsertionEffect::Impl::apply_stereo_eq()
{
    apply_common_tail();

    // Low shelf: parameter 0 picks the frequency, latched to 0/1, and 1 is the gain. Both channels
    // are programmed from the same pair.
    if (params[0] < 2) {
        eq_freq_latch[0] = params[0];
    } else {
        params[0] = eq_freq_latch[0];
    }
    if (changed(0) || changed(1)) {
        const std::size_t g = params[1];
        const bool alternate = eq_freq_latch[0] != 0;
        const std::uint16_t f = alternate ? eq_low_alt_f[g] : low_gain_f[g];
        const std::uint16_t d = alternate ? eq_low_alt_d[g] : low_gain_d[g];
        const std::uint16_t h = alternate ? eq_low_alt_h[g] : low_gain_h[g];
        for (const int base : {0x8D, 0xC2}) {
            registers.write16(base, f);
            registers.write16(base + 2, d);
            registers.write16(base + 4, h);
        }
        shadow[0] = eq_freq_latch[0];
        shadow[1] = params[1];
    }

    // High shelf: parameter 2 the frequency, 3 the gain, bracketed by its two mute registers.
    if (params[2] < 2) {
        eq_freq_latch[1] = params[2];
    } else {
        params[2] = eq_freq_latch[1];
    }
    if (changed(2) || changed(3)) {
        registers.write_slew(0xBB, 0);
        registers.write_slew(0xF0, 0);
        const std::size_t g = params[3];
        const bool alternate = eq_freq_latch[1] != 0;
        const std::uint16_t f = alternate ? eq_high_alt_f[g] : high_gain_f[g];
        const std::uint16_t d = alternate ? eq_high_alt_d[g] : high_gain_d[g];
        const std::uint16_t h = alternate ? eq_high_alt_h[g] : high_gain_h[g];
        for (const int base : {0x95, 0xCA}) {
            registers.write16(base, f);
            registers.write16(base + 2, d);
            registers.write16(base + 4, h);
        }
        registers.write_slew(0xBB, 0x20);
        registers.write_slew(0xF0, 0x20);
        shadow[2] = eq_freq_latch[1];
        shadow[3] = params[3];
    }

    // The two mid bands. Each is programmed twice, once per channel, and the first of the pair
    // takes the latched Q while the second takes the raw byte — the engine's own asymmetry, and
    // it only shows when the Q byte is out of range.
    for (int band = 0; band < 2; ++band) {
        const int frequency_param = band == 0 ? 4 : 7;
        const int q_param = band == 0 ? 5 : 8;
        const int gain_param = band == 0 ? 6 : 9;
        auto& latch = eq_q_latch[static_cast<std::size_t>(band)];
        if (params[static_cast<std::size_t>(q_param)] < 5) {
            latch = params[static_cast<std::size_t>(q_param)];
        } else {
            params[static_cast<std::size_t>(q_param)] = latch;
        }
        if (!changed(frequency_param) && !changed(q_param) && !changed(gain_param)) {
            continue;
        }
        const std::uint8_t frequency = params[static_cast<std::size_t>(frequency_param)];
        const std::uint8_t gain = params[static_cast<std::size_t>(gain_param)];
        bank_load(eq_mid_registers[static_cast<std::size_t>(band * 2)], frequency, latch, gain);
        bank_load(eq_mid_registers[static_cast<std::size_t>((band * 2) + 1)],
                  frequency,
                  params[static_cast<std::size_t>(q_param)],
                  gain);
        shadow[static_cast<std::size_t>(frequency_param)] = frequency;
        shadow[static_cast<std::size_t>(q_param)] = latch;
        shadow[static_cast<std::size_t>(gain_param)] = gain;
    }

    const auto level = static_cast<std::size_t>(std::clamp<int>(params[0x13], 0, 0x7F));
    registers.write_slew(0x80, level_curve[level]);
    registers.write_slew(0x1F0, level_curve[level]);
    shadow[0x13] = params[0x13];
}

/// `fx_param_apply_57bc0`, the Rotary handler. Parameters 0/1 are the two rotors' slow and fast
/// speeds and 4/5 their fast counterparts; 10 selects which pair is live, so the *speed switch*
/// picks between two stored rates rather than sweeping one.
void InsertionEffect::Impl::apply_rotary(bool on_select)
{
    // Selecting the type poisons these two shadow bytes rather than leaving them at the defaults
    // just copied in, which is what makes both rotor rates re-apply on a fresh selection even
    // when the new defaults happen to match the latched values.
    if (on_select) {
        shadow[2] = 0xFF;
        shadow[6] = 0xFF;
    }

    apply_common_tail();

    if (changed(0x10)) {
        const std::size_t v = params[0x10];
        for (const int base : {0x17C, 0x195}) {
            registers.write16(base, low_gain_f[v]);
            registers.write16(base + 2, low_gain_d[v]);
            registers.write16(base + 4, low_gain_h[v]);
        }
        shadow[0x10] = params[0x10];
    }
    if (changed(0x11)) {
        registers.write_slew(0x18A, 0);
        registers.write_slew(0x1A3, 0);
        const std::size_t v = params[0x11];
        registers.write16(0x184, high_gain_f[v]);
        registers.write16(0x182, high_gain_d[v]);
        registers.write16(0x186, high_gain_h[v]);
        registers.write16(0x19D, high_gain_f[v]);
        registers.write16(0x19B, high_gain_d[v]);
        registers.write16(0x19F, high_gain_h[v]);
        registers.write_slew(0x18A, 0x20);
        registers.write_slew(0x1A3, 0x20);
        shadow[0x11] = params[0x11];
    }

    // The two rotors' rate registers and their depths.
    if (changed(2)) {
        registers.write16(0x139, rotary_rate[params[2]]);
        shadow[2] = params[2];
    }
    if (changed(3)) {
        registers.write_slew(0xB2, level_curve[params[3]]);
        shadow[3] = params[3];
    }
    if (changed(6)) {
        registers.write16(0xBE, rotary_rate[params[6]]);
        shadow[6] = params[6];
    }
    if (changed(7)) {
        registers.write_slew(0xA6, level_curve[params[7]]);
        shadow[7] = params[7];
    }
    if (changed(8)) {
        registers.write_slew(0xF1, level_curve[params[8]]);
        registers.write_slew(0x125, level_curve[params[8]]);
        registers.write_slew(0xF2, rotary_spread[params[8]]);
        registers.write_slew(0x126, rotary_spread[params[8]]);
        shadow[8] = params[8];
    }

    // The speed switch, which also runs in the control-only mode. Below the midpoint the slow
    // pair is live, at or above it the fast pair -- and the two rotors take their speed from
    // different parameters, which is why the horn and the drum change over together but not by
    // the same amount.
    const int selected = std::clamp(static_cast<int>(params[10]), 0, 0x7F);
    const std::size_t horn = selected < 0x40 ? params[4] : params[5];
    const std::size_t drum = selected < 0x40 ? params[0] : params[1];
    registers.write16(0xB6, rotary_speed[horn]);
    registers.write16(0xBA, rotary_speed[horn]);
    registers.write16(0x131, rotary_speed[drum]);
    registers.write16(0x135, rotary_speed[drum]);
    shadow[10] = params[10];

    const auto level = static_cast<std::size_t>(std::clamp<int>(params[0x13], 0, 0x7F));
    registers.write_slew(0x80, level_curve[level]);
    registers.write_slew(0x1F0, level_curve[level]);
    shadow[0x13] = params[0x13];
}

/// `fx_param_apply_51be0`, the OD / OD2 handler. Two chains, laid out identically in the GS
/// parameter block: chain one takes params 0/1/2/3 (type, drive, amp type, amp switch) with pan at
/// 0x0F and level at 0x10, chain two takes 5/6/7/8 with pan 0x11 and level 0x12.
void InsertionEffect::Impl::apply_od_od2()
{
    apply_common_tail();

    if (changed(0x13)) {
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
        shadow[0x13] = params[0x13];
    }

    // Chain one, then chain two. The OD type is latched to 0/1 and the amp type to 0-3, the same
    // read-back-the-latch clamp the other handlers use.
    od2_chain(0, 0, 1, 0x18);
    od2_chain(1, 5, 6, 0x19);

    // Amp switch and amp type, per chain.
    for (const int chain : {0, 1}) {
        const int switch_param = chain == 0 ? 3 : 8;
        const int type_param = chain == 0 ? 2 : 7;
        auto& switch_latch = od2_amp_switch_latch[static_cast<std::size_t>(chain)];
        auto& type_latch = od2_amp_type_latch[static_cast<std::size_t>(chain)];
        if (params[static_cast<std::size_t>(switch_param)] < 2) {
            switch_latch = params[static_cast<std::size_t>(switch_param)];
        } else {
            params[static_cast<std::size_t>(switch_param)] = switch_latch;
        }
        if (changed(switch_param)) {
            registers.write_slew(chain == 0 ? 0x108 : 0x18A, amp_switch_a[switch_latch]);
            registers.write_slew(chain == 0 ? 0x107 : 0x189, amp_switch_b[switch_latch]);
            shadow[static_cast<std::size_t>(switch_param)] = switch_latch;
        }
        if (params[static_cast<std::size_t>(type_param)] < 4) {
            type_latch = params[static_cast<std::size_t>(type_param)];
        } else {
            params[static_cast<std::size_t>(type_param)] = type_latch;
        }
        if (changed(type_param)) {
            amp_bank_program(chain == 0 ? od2_amp_bank_a : od2_amp_bank_b, type_latch, switch_latch);
            shadow[static_cast<std::size_t>(type_param)] = type_latch;
        }
    }

    // Pan and level, per chain.
    if (changed(0x0F)) {
        registers.write_slew(0x191, pan_left[params[0x0F]]);
        registers.write_slew(0x196, pan_right[params[0x0F]]);
        shadow[0x0F] = params[0x0F];
    }
    if (changed(0x10)) {
        registers.write_slew(0x18E, level_curve[params[0x10]]);
        shadow[0x10] = params[0x10];
    }
    if (changed(0x11)) {
        registers.write_slew(0x192, pan_left[params[0x11]]);
        registers.write_slew(0x197, pan_right[params[0x11]]);
        shadow[0x11] = params[0x11];
    }
    if (changed(0x12)) {
        registers.write_slew(0x18F, level_curve[params[0x12]]);
        shadow[0x12] = params[0x12];
    }

    // The drive pair runs in the control-only mode too, carrying the EFX control offsets -- zero
    // here until control sources are modelled. Which second curve it uses depends on the chain's
    // OD type, and the two chains disagree about which way round that test goes.
    const auto drive_pair = [&](int chain, int drive_param) {
        const auto v = static_cast<std::size_t>(
            std::clamp<int>(params[static_cast<std::size_t>(drive_param)], 0, 0x7F));
        // Which second drive curve the chain uses turns on its OD type -- and the two chains
        // test it opposite ways round: chain one takes the alternate curve when its type is 1,
        // chain two whenever its type is anything but 0.
        const std::uint8_t type = od2_type_latch[static_cast<std::size_t>(chain)];
        const bool alternate = chain == 0 ? type == 1 : type != 0;
        const std::uint8_t second = alternate ? od2_drive_alt[v] : drive_b[v];
        registers.write_pair(chain == 0 ? 0xA1 : 0x123, drive_a[v], chain == 0 ? 0xD5 : 0x157,
                             second);
        shadow[static_cast<std::size_t>(drive_param)] = params[static_cast<std::size_t>(drive_param)];
    };
    drive_pair(0, 1);
    drive_pair(1, 6);
}

/// One chain's OD type: latched to 0/1, and on a change it programs that chain's nineteen-register
/// bank from the shared tables, brackets the change with the chain's mute registers, and refreshes
/// its drive pair and level.
void InsertionEffect::Impl::od2_chain(int chain, int type_param, int drive_param, int)
{
    auto& latch = od2_type_latch[static_cast<std::size_t>(chain)];
    if (params[static_cast<std::size_t>(type_param)] < 2) {
        latch = params[static_cast<std::size_t>(type_param)];
    } else {
        params[static_cast<std::size_t>(type_param)] = latch;
    }
    if (!changed(type_param)) {
        return;
    }

    const int mute_level = chain == 0 ? 0x18E : 0x18F;
    const int mute_bank = chain == 0 ? 0x8A : 0x10C;
    registers.write_slew(mute_level, 0);
    registers.write_slew(mute_bank, 0);

    const auto& bank_registers = chain == 0 ? od2_bank_registers_a : od2_bank_registers_b;
    for (std::size_t i = 0; i < bank_registers.size(); ++i) {
        // These are raw `fx_reg_write` indices, so they carry no 0x80 bias.
        const std::uint8_t value = od2_bank[i][latch];
        registers.write_index_byte(bank_registers[i], value);
    }

    const auto v = static_cast<std::size_t>(
        std::clamp<int>(params[static_cast<std::size_t>(drive_param)], 0, 0x7F));
    const bool alternate = chain == 0 ? latch == 1 : latch != 0;
    const std::uint8_t second = alternate ? od2_drive_alt[v] : drive_b[v];
    registers.write_pair(chain == 0 ? 0xA1 : 0x123, drive_a[v], chain == 0 ? 0xD5 : 0x157, second);
    registers.write_slew(mute_bank, 0x20);
    registers.write_slew(mute_level, level_curve[params[chain == 0 ? 0x10 : 0x12]]);
    shadow[static_cast<std::size_t>(type_param)] = latch;
}

/// `fx_process` @ 0x180056560, the EFX Reverb's apply handler. The 20 GS parameters: 1 is the
/// Character (0–5, latched), 2 the pre-LPF, 3 the time (control-modulated in the engine), 4 the
/// delay feedback, 16 a second control-modulated pair, 17/18 the gain shelves shared with
/// Overdrive's curves, 20 the level.
void InsertionEffect::Impl::apply_efx_reverb()
{
    apply_common_tail();

    if (changed(0x10)) {
        const std::size_t v = params[0x10];
        registers.write16(0x1BA, low_gain_f[v]);
        registers.write16(0x1B8, low_gain_d[v]);
        registers.write16(0x1BC, low_gain_h[v]);
        registers.write16(0x1CA, low_gain_f[v]);
        registers.write16(0x1C8, low_gain_d[v]);
        registers.write16(0x1CC, low_gain_h[v]);
        shadow[0x10] = params[0x10];
    }
    if (changed(0x11)) {
        registers.write_slew(0x80, 0);
        registers.write_slew(0x1F0, 0);
        const std::size_t v = params[0x11];
        registers.write16(0x1C0, high_gain_f[v]);
        registers.write16(0x1BE, high_gain_d[v]);
        registers.write16(0x1C2, high_gain_h[v]);
        registers.write16(0x1D0, high_gain_f[v]);
        registers.write16(0x1CE, high_gain_d[v]);
        registers.write16(0x1D2, high_gain_h[v]);
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
        shadow[0x11] = params[0x11];
    }
    if (changed(0x13)) {
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
        shadow[0x13] = params[0x13];
    }

    // The Character, 0–5 and latched — out-of-range writes read back the latch.
    if (params[0] < 6) {
        latch_dea0 = params[0];
    } else {
        params[0] = latch_dea0;
    }
    if (changed(0)) {
        reverb_character_program();
        shadow[0] = latch_dea0;
    }

    if (changed(1)) {
        registers.write_slew(0xB5, 0);
        registers.write_lfo5(0xA9,
                             static_cast<std::uint16_t>(rev_lpf_curve[params[1]] + 0x6000));
        amp_type_latch = params[1];
        registers.write_slew(0xB5, 0x7F);
        // The staircase the lfo5 fields encode slews the pre-delay tap toward this position; the
        // end state is the position itself, which is also the factory seed (curve[0x70]+0x6000).
        reverb_taps[0] = rev_lpf_curve[params[1]] + 0x6000;
        shadow[1] = params[1];
    }
    if (changed(3)) {
        registers.write16(0xF5, 0);
        registers.write16(0x137, 0);
        const std::size_t v = params[3];
        registers.write16(0xF1, rev_feedback_a[v]);
        registers.write16(0x133, rev_feedback_a[v]);
        registers.write16(0xEE, rev_feedback_b[v]);
        registers.write16(0x130, rev_feedback_b[v]);
        const auto tv = static_cast<std::size_t>(std::clamp<int>(params[2], 0, 0x7F));
        registers.write16(0xF5, rev_time_curve[tv]);
        registers.write16(0x137, rev_time_curve[tv]);
        shadow[3] = params[3];
    }

    // The tail runs in the control-only mode too: time and the p16 pair carry the EFX control
    // offsets in the engine, zero here until control sources are modelled.
    const auto tv = static_cast<std::size_t>(std::clamp<int>(params[2], 0, 0x7F));
    registers.write16(0xF5, rev_time_curve[tv]);
    registers.write16(0x137, rev_time_curve[tv]);
    amp_switch_latch = params[2];
    shadow[2] = params[2];
    const auto pv = static_cast<std::size_t>(std::clamp<int>(params[0xF], 0, 0x7F));
    registers.write_slew(0x18C, rev_p16_a[pv]);
    registers.write_slew(0x1B4, rev_p16_a[pv]);
    registers.write_slew(0x18B, rev_p16_b[pv]);
    registers.write_slew(0x1B3, rev_p16_b[pv]);
    shadow[0xF] = params[0xF];
}

/// The Character branch: bank defaults, then the character's own values and tap program over the
/// top. A latch past 5 falls back to the spare tables and pins the character at 3.
void InsertionEffect::Impl::reverb_character_program()
{
    registers.write_slew(0x9F, 0);
    registers.write_slew(0x189, 0);
    registers.write_slew(0x1B1, 0);
    reverb_taps[0] = 0x6001;

    registers.write16(rev_regs16[0], rev_defaults[0]);
    registers.write16(rev_regs16[1], rev_defaults[1]);
    for (int j = 0; j < 17; ++j) {
        registers.write_low(rev_regs8[static_cast<std::size_t>(j)],
                            static_cast<std::uint8_t>(rev_defaults[static_cast<std::size_t>(2 + j)]
                                                      & 0xFF));
    }
    for (int j = 0; j < 32; ++j) {
        reverb_taps[static_cast<std::size_t>(2 + j)] =
            rev_default_taps[static_cast<std::size_t>(j)];
    }

    const std::uint8_t character = latch_dea0;
    const std::uint16_t* taps = rev_fallback_taps.data();
    const std::uint16_t* values = rev_fallback_values.data();
    if (character < 6) {
        taps = rev_char_taps[character].data();
        values = rev_char_values[character].data();
    } else {
        params[0] = 3;
        latch_dea0 = 3;
    }
    registers.write16(rev_regs16[0], values[0]);
    registers.write16(rev_regs16[1], values[1]);
    for (int j = 0; j < 16; ++j) {
        registers.write_low(rev_regs8[static_cast<std::size_t>(j)],
                            static_cast<std::uint8_t>(values[2 + j] & 0xFF));
    }
    for (int j = 0; j < 32; ++j) {
        reverb_taps[static_cast<std::size_t>(2 + j)] = taps[j];
    }

    registers.write_slew(0xB5, 0);
    registers.write_lfo5(0xA9, static_cast<std::uint16_t>(rev_lpf_curve[params[1]] + 0x6000));
    registers.write_slew(0xB5, 0x7F);
    reverb_taps[0] = rev_lpf_curve[params[1]] + 0x6000;
    registers.write_slew(0x9F, static_cast<std::uint8_t>(values[18] & 0xFF));
    registers.write_slew(0x189, 0x20);
    registers.write_slew(0x1B1, 0x20);
}

// ---------------------------------------------------------------------------------------------

InsertionEffect::InsertionEffect(const RomImage& rom) : impl_(std::make_unique<Impl>(rom))
{
    // Power-on. The engine's factory state is the EFX Reverb's: its character-3 tap program and
    // pre-delay sit in `.data`, and the parameter latches hold its defaults — which is what makes
    // a later `01 55` selection leave the gain registers at their preset values. Selecting the
    // reverb against the zeroed latches and then Thru on top reproduces that state exactly
    // (register file verified word-for-word against `scdec efxdump` for Thru, Overdrive and the
    // reverb).
    impl_->registers.set_width_map(impl_->width_maps[0].data());
    impl_->registers.seed_startup();
    impl_->reverb_taps[1] = 4800;
    impl_->select_key(0x0155);
    impl_->select_key(0x0000);
}

InsertionEffect::~InsertionEffect() = default;
InsertionEffect::InsertionEffect(InsertionEffect&&) noexcept = default;
InsertionEffect& InsertionEffect::operator=(InsertionEffect&&) noexcept = default;

const std::vector<EfxRecord>& InsertionEffect::directory() const
{
    return impl_->directory;
}

void InsertionEffect::select_type(int msb, int lsb)
{
    impl_->select_key(static_cast<std::uint16_t>(((msb & 0xFF) << 8) | (lsb & 0xFF)));
}

void InsertionEffect::set_parameter(int address, int value)
{
    const auto byte = static_cast<std::uint8_t>(std::clamp(value, 0, 0x7F));
    int index = -1;
    if (address >= 0x03 && address <= 0x16) {
        index = address - 0x03; // the 20 parameters
    } else if (address >= 0x17 && address <= 0x1A) {
        index = address - 0x17 + 0x14; // sends and the routing byte
    } else if (address >= 0x1B && address <= 0x1E) {
        index = address - 0x1B + 0x1C; // control sources and depths
    }
    if (index < 0) {
        return;
    }
    impl_->params[static_cast<std::size_t>(index)] = byte;
    impl_->apply(false);
}

const EfxRecord& InsertionEffect::current() const
{
    return impl_->directory[static_cast<std::size_t>(impl_->record_index)];
}

bool InsertionEffect::implemented() const
{
    return impl_->processor != Processor::passthrough;
}

// The send ramps convert their u16 targets as `value × 3.0517578e-05 × 2` — target/16384, so a
// full-scale send byte is 16256/16384 ≈ 0.992 on the block's stereo sum (`fx_process_block`, the
// ramp loop ahead of the accumulates, where the source is `out_left + out_right`).
//
// Tapping the *sum* rather than a mono signal is the block's own behaviour and is measurable: in
// the live engine an EFX send scales with the part's pan — centre/hard = 1.1825 against the pan
// table's centre sum of 2×75/127 = 1.1811 — where a part send is pan-independent (1.0000), since
// that one is fed pre-pan. The gain law and the tap are therefore both traced.
//
// What the raw fraction is *not* is a gain in this engine's units. Each network here carries its
// own input constant (`ReverbPresets::send_at_full_scale` and friends), so an engine bus value
// has to be converted per network. The reverb conversion below is measured rather than derived:
// with a level-transparent Thru block in the path, the live engine puts 0.980 of a part send's
// wet on the reverb bus for the same byte, where this engine put 1.144 — so the raw fraction is
// scaled to bring the two into the engine's ratio.
//
// It is a ratio against the part send rather than a constant fitted to render levels, because a
// ratio between two paths through the same network cancels everything the two share. That matters:
// the wet level measured off a rendered note also carries the dry signal that fed the reverb, which
// differs a little between engines and varies with the patch (+-11% across four), so an absolute
// constant fitted that way would be fitting the voice path as much as the send. The ratio measured
// flat across the whole send sweep where absolute levels did not, which is the sign it is the right
// quantity.
//
// The network itself is not in question: its impulse response, taken from the live engine by
// calling the module's own reverb processor (`scdec revir`), matches this one to 1.0000 in every
// window of a 32000-sample response, peaks agreeing to five figures.
double InsertionEffect::reverb_send() const noexcept
{
    constexpr double bus_conversion = 0.857;
    return (impl_->send_reverb / 16384.0) * bus_conversion;
}

// No conversion on these two: the delay bus lands within 5% of the live engine on the raw
// fraction, and the chorus bus has no measurement behind it at all — the GS default chorus is
// silent on the probes that pinned the other two, so a constant here would be invention.
double InsertionEffect::chorus_send() const noexcept
{
    return impl_->send_chorus / 16384.0;
}

double InsertionEffect::delay_send() const noexcept
{
    return impl_->send_delay / 16384.0;
}

void InsertionEffect::process(std::span<const float> in_left,
                              std::span<const float> in_right,
                              std::span<float> out_left,
                              std::span<float> out_right)
{
    Impl& impl = *impl_;
    const float* coef = impl.registers.coef.data();
    // No make-up gain: the ×4 an earlier revision calibrated here turned out to live in the
    // registers themselves — the startup words give the routing pair at 0x83/0x1F3 the wide
    // scale, so the normal routing byte 0x7F means ×3.97 inside the algorithm. With the register
    // file matching the live engine word-for-word, a Thru-routed part is level-transparent at
    // unity return.
    //
    // The routing byte is a crossfade pair, not a mute: its two u16 ramp targets swap between
    // (return 1.0, bypass 0) and (return 0, bypass 1.0), so EFX-off passes the input through dry.
    const float wet = impl.output_enabled ? 1.0F : 0.0F;
    const float bypass = impl.output_enabled ? 0.0F : 1.0F;

    for (std::size_t n = 0; n < in_left.size(); ++n) {
        if (impl.processor == Processor::passthrough) {
            // An untranscribed type is a unity bypass either way.
            out_left[n] = in_left[n];
            out_right[n] = in_right[n];
            continue;
        }
        // The engine wraps both delay lines, then runs the algorithm on doubled inputs and halves
        // what comes back.
        impl.tape_a.step();
        impl.tape_b.step();
        float left = 0.0F;
        float right = 0.0F;
        if (impl.processor == Processor::thru) {
            thru_sample(in_left[n] + in_left[n],
                        in_right[n] + in_right[n],
                        left,
                        right,
                        impl.tape_a,
                        coef);
        } else if (impl.processor == Processor::stereo_eq) {
            stereo_eq_sample(in_left[n] + in_left[n],
                             in_right[n] + in_right[n],
                             left,
                             right,
                             impl.tape_a,
                             coef);
        } else if (impl.processor == Processor::enhancer) {
            enhancer_sample(in_left[n] + in_left[n],
                            in_right[n] + in_right[n],
                            left,
                            right,
                            impl.tape_a,
                            coef);
        } else if (impl.processor == Processor::hexa_chorus) {
            hexa_chorus_sample(in_left[n] + in_left[n],
                               in_right[n] + in_right[n],
                               left,
                               right,
                               impl.tape_a,
                               impl.tape_b,
                               coef);
        } else if (impl.processor == Processor::space_d) {
            space_d_sample(in_left[n] + in_left[n],
                           in_right[n] + in_right[n],
                           left,
                           right,
                           impl.tape_a,
                           impl.tape_b,
                           coef);
        } else if (impl.processor == Processor::rotary) {
            rotary_sample(in_left[n] + in_left[n],
                          in_right[n] + in_right[n],
                          left,
                          right,
                          impl.tape_a,
                          impl.tape_b,
                          coef);
        } else if (impl.processor == Processor::od_od2) {
            od_od2_sample(in_left[n] + in_left[n],
                          in_right[n] + in_right[n],
                          left,
                          right,
                          impl.tape_a,
                          coef);
        } else if (impl.processor == Processor::efx_reverb) {
            reverb_sample(in_left[n] + in_left[n],
                          in_right[n] + in_right[n],
                          left,
                          right,
                          impl.tape_a,
                          impl.tape_b,
                          impl.reverb_taps.data(),
                          coef);
        } else {
            overdrive_sample(in_left[n] + in_left[n],
                             in_right[n] + in_right[n],
                             left,
                             right,
                             impl.tape_a,
                             coef);
        }
        out_left[n] = left * 0.5F * wet + in_left[n] * bypass;
        out_right[n] = right * 0.5F * wet + in_right[n] * bypass;
    }
}

void InsertionEffect::reset()
{
    impl_->tape_a.clear();
    impl_->tape_b.clear();
}

std::span<const float> InsertionEffect::coefficients() const
{
    return {impl_->registers.coef.data(), static_cast<std::size_t>(register_count)};
}

std::span<const std::int32_t> InsertionEffect::tap_program() const
{
    return {impl_->reverb_taps.data(), impl_->reverb_taps.size()};
}

} // namespace ts
