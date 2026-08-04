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
    explicit Tape(int floats) : ring_(static_cast<std::size_t>(floats), 0.0F), mask_(floats - 1) {}

    [[nodiscard]] float& at(int byte_offset) noexcept
    {
        return ring_[static_cast<std::size_t>((base_ + (byte_offset >> 2)) & mask_)];
    }

    void step() noexcept { base_ = (base_ - 1) & mask_; }

    void clear() noexcept { std::fill(ring_.begin(), ring_.end(), 0.0F); }

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

/// Which transcription serves a dispatch index.
enum class Processor {
    thru, ///< dispatch 0, and dispatch 1's cleared "no effect" state
    overdrive, ///< dispatch 3 and 4 — one dataflow, two presets
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
    std::array<std::array<std::uint16_t, 4>, 16> amp_bank_values{};

    RegisterFile registers;
    Tape tape_a{1 << 15};
    Tape tape_b{1 << 17};

    std::array<std::uint8_t, parameter_bytes> params{};
    std::array<std::uint8_t, parameter_bytes> shadow{};
    bool force_apply = true;

    int record_index = 0;
    int dispatch = 0;
    Processor processor = Processor::thru;

    // The apply handlers' clamped-mode latches (`DAT_181a1dea1/dea2/deb8`).
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
    void apply(bool force);
    void apply_common_tail();
    void apply_overdrive(bool force);
    void amp_bank_program(std::uint8_t type, std::uint8_t simulator);

    [[nodiscard]] bool changed(int index, bool force) const noexcept
    {
        return force
               || params[static_cast<std::size_t>(index)]
                      != shadow[static_cast<std::size_t>(index)];
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

void InsertionEffect::Impl::apply(bool force)
{
    force = force || force_apply;
    switch (processor) {
    case Processor::overdrive:
        apply_overdrive(force);
        break;
    case Processor::thru:
    case Processor::passthrough:
        // Thru's handler (`fx_param_apply_5aab0`) sets its two level registers on commit and then
        // runs the common tail; types without a transcription still get the tail, so their sends
        // and routing stay live.
        if (force) {
            registers.write_slew(0x80, 0x7F);
            registers.write_slew(0x1F0, 0x7F);
        }
        apply_common_tail();
        break;
    }
    shadow = params;
    force_apply = false;
}

/// The send and routing bytes every apply handler ends with.
void InsertionEffect::Impl::apply_common_tail()
{
    send_reverb = static_cast<std::uint16_t>(params[0x14] << 7);
    send_chorus = static_cast<std::uint16_t>(params[0x15] << 7);
    send_delay = static_cast<std::uint16_t>(params[0x16] << 7);
    if (params[0x1A] < 2) {
        output_latch = params[0x1A];
    } else {
        params[0x1A] = output_latch;
    }
    output_enabled = output_latch != 0;
}

/// `fx_param_apply_53a10`, the Overdrive/Distortion handler.
void InsertionEffect::Impl::apply_overdrive(bool force)
{
    apply_common_tail();

    // Tone shelves, 16-bit filter coefficients from the gain curves.
    if (changed(0x10, force)) {
        const std::size_t v = params[0x10];
        registers.write16(0x10F, low_gain_f[v]);
        registers.write16(0x10D, low_gain_d[v]);
        registers.write16(0x111, low_gain_h[v]);
    }
    if (changed(0x11, force)) {
        registers.write_slew(0x80, 0);
        registers.write_slew(0x1F0, 0);
        const std::size_t v = params[0x11];
        registers.write16(0x115, high_gain_f[v]);
        registers.write16(0x113, high_gain_d[v]);
        registers.write16(0x117, high_gain_h[v]);
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
    }
    if (changed(0x13, force)) {
        registers.write_slew(0x80, level_curve[params[0x13]]);
        registers.write_slew(0x1F0, level_curve[params[0x13]]);
    }

    // Amp simulator switch (0/1, latched) and amp type (0–3, latched).
    if (params[2] < 2) {
        amp_switch_latch = params[2];
    } else {
        params[2] = amp_switch_latch;
    }
    if (changed(2, force)) {
        registers.write_slew(0x10C, amp_switch_a[amp_switch_latch]);
        registers.write_slew(0x10B, amp_switch_b[amp_switch_latch]);
    }
    if (params[1] < 4) {
        amp_type_latch = params[1];
    } else {
        params[1] = amp_type_latch;
    }
    if (changed(1, force)) {
        amp_bank_program(amp_type_latch, amp_switch_latch);
    }

    // Drive and pan, always refreshed — in the engine the EFX control offsets ride in here, which
    // is why these two run even in the control-only mode. Control sources are not modulated yet,
    // so the offsets are zero.
    const auto drive = static_cast<std::size_t>(std::clamp<int>(params[0], 0, 0x7F));
    registers.write_pair(0xA5, drive_a[drive], 0xD9, drive_b[drive]);
    const auto pan = static_cast<std::size_t>(std::clamp<int>(params[0x12], 0, 0x7F));
    registers.write_slew(0x11B, pan_left[pan]);
    registers.write_slew(0x11C, pan_right[pan]);
}

/// `chorus_load_algo_regs` @ 0x180005860, programming the amp-simulator register bank.
void InsertionEffect::Impl::amp_bank_program(std::uint8_t type, std::uint8_t simulator)
{
    if (type >= 4) {
        return;
    }
    const std::uint8_t sw = std::min<std::uint8_t>(simulator, 1);
    const auto& r = amp_bank_registers;
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

// ---------------------------------------------------------------------------------------------

InsertionEffect::InsertionEffect(const RomImage& rom) : impl_(std::make_unique<Impl>(rom))
{
    // Power-on state: Thru, with its defaults live.
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

double InsertionEffect::reverb_send() const noexcept
{
    return impl_->send_reverb / 32768.0;
}

double InsertionEffect::chorus_send() const noexcept
{
    return impl_->send_chorus / 32768.0;
}

double InsertionEffect::delay_send() const noexcept
{
    return impl_->send_delay / 32768.0;
}

void InsertionEffect::process(std::span<const float> in_left,
                              std::span<const float> in_right,
                              std::span<float> out_left,
                              std::span<float> out_right)
{
    Impl& impl = *impl_;
    const float* coef = impl.registers.coef.data();
    // The return gain is calibrated against the live engine, not derived: rendering the same
    // probe through the DLL (`scdec smf`), a Thru-routed part is level-transparent and Overdrive
    // lands within 1.3% of this ×4 — half of it is the engine's own dry-return ramp target (2.0,
    // `fx_param_apply` writes 0x40000000), the other half sits in the send-matrix taps this
    // engine abstracts away. Placing it all on the return and none on the input is itself a
    // measured fact: the clipper's drive already matches at unity input.
    const float output = impl.output_enabled ? 4.0F : 0.0F;

    for (std::size_t n = 0; n < in_left.size(); ++n) {
        if (impl.processor == Processor::passthrough) {
            // An untranscribed type is a unity bypass — only the routing toggle applies.
            out_left[n] = impl.output_enabled ? in_left[n] : 0.0F;
            out_right[n] = impl.output_enabled ? in_right[n] : 0.0F;
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
        } else {
            overdrive_sample(in_left[n] + in_left[n],
                             in_right[n] + in_right[n],
                             left,
                             right,
                             impl.tape_a,
                             coef);
        }
        out_left[n] = left * 0.5F * output;
        out_right[n] = right * 0.5F * output;
    }
}

void InsertionEffect::reset()
{
    impl_->tape_a.clear();
    impl_->tape_b.clear();
}

} // namespace ts
