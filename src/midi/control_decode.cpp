#include "tabulasonora/control_decode.hpp"

#include "tabulasonora/sequence.hpp"

namespace ts {
namespace {

[[nodiscard]] ControlUpdate part_update(ControlTarget target, int channel, int value) noexcept
{
    return ControlUpdate{target, channel, value};
}

[[nodiscard]] ControlUpdate global_update(ControlTarget target, int value) noexcept
{
    return ControlUpdate{target, -1, value};
}

} // namespace

std::optional<ControlUpdate> decode_control_change(int channel, int controller, int value) noexcept
{
    switch (controller) {
    case 0:
        return part_update(ControlTarget::bank, channel, value);
    case 1:
        return part_update(ControlTarget::modulation, channel, value);
    case 7:
        return part_update(ControlTarget::volume, channel, value);
    // Zero folds to one: the wheel cannot reach the random pan position, which only the GS SysEx
    // panpot can write.
    case 10:
        return part_update(ControlTarget::pan, channel, value == 0 ? 1 : value);
    case 11:
        return part_update(ControlTarget::expression, channel, value);

    // The sound controllers, all eight.
    case 71:
        return part_update(ControlTarget::tvf_resonance, channel, value);
    case 72:
        return part_update(ControlTarget::env_release, channel, value);
    case 73:
        return part_update(ControlTarget::env_attack, channel, value);
    case 74:
        return part_update(ControlTarget::tvf_cutoff, channel, value);
    case 75:
        return part_update(ControlTarget::env_decay, channel, value);
    case 76:
        return part_update(ControlTarget::vibrato_rate, channel, value);
    case 77:
        return part_update(ControlTarget::vibrato_depth, channel, value);
    case 78:
        return part_update(ControlTarget::vibrato_delay, channel, value);

    case 64:
        return part_update(ControlTarget::damper, channel, value);
    case 91:
        return part_update(ControlTarget::reverb_send, channel, value);
    case 93:
        return part_update(ControlTarget::chorus_send, channel, value);
    case 94:
        return part_update(ControlTarget::delay_send, channel, value);
    default:
        return std::nullopt;
    }
}

bool gs_checksum_ok(std::span<const std::uint8_t> bytes) noexcept
{
    // F0 41 dev 42 12 <3-byte address> <data...> checksum F7.
    if (bytes.size() < 11 || bytes[0] != 0xF0 || bytes[1] != 0x41 || bytes[3] != 0x42
        || bytes[4] != 0x12) {
        return false;
    }

    unsigned int sum = 0;
    for (std::size_t i = 5; i + 1 < bytes.size(); ++i) {
        sum += bytes[i];
    }
    return sum % 0x80 == 0;
}

std::optional<ControlUpdate> decode_gs_sysex(std::span<const std::uint8_t> bytes, int port) noexcept
{
    if (!gs_checksum_ok(bytes)) {
        return std::nullopt;
    }

    const int a1 = bytes[5];
    const int a2 = bytes[6];
    const int a3 = bytes[7];
    const int value = bytes[8];

    // The `50`/`51` blocks are the second port's mirrors of `40`/`41`.
    const bool second_port = a1 == 0x50 || a1 == 0x51;
    if (a1 != 0x40 && !second_port) {
        return std::nullopt;
    }

    const int block_port = second_port ? 1 : port;
    const auto part_of = [block_port](int block) {
        return (block_port * Sequence::channel_count) + sequence_builder::channel_from_block(block);
    };

    // The system EQ block, which belongs to the module rather than to a part.
    if (a2 == 0x02) {
        switch (a3) {
        case 0x00:
            return global_update(ControlTarget::eq_low_frequency, value);
        case 0x01:
            return global_update(ControlTarget::eq_low_gain, value);
        case 0x02:
            return global_update(ControlTarget::eq_high_frequency, value);
        case 0x03:
            return global_update(ControlTarget::eq_high_gain, value);
        default:
            return std::nullopt;
        }
    }

    // Part parameters.
    if ((a2 & 0xF0) == 0x10) {
        const int channel = part_of(a2 & 0x0F);
        switch (a3) {
        case 0x1A:
            return part_update(ControlTarget::velocity_depth, channel, value);
        case 0x1B:
            return part_update(ControlTarget::velocity_offset, channel, value);
        case 0x2C:
            return part_update(ControlTarget::delay_send, channel, value);
        // The modify block. Its order is not the NRPNs': vibrato delay is last here and third
        // there.
        case 0x30:
            return part_update(ControlTarget::vibrato_rate, channel, value);
        case 0x31:
            return part_update(ControlTarget::vibrato_depth, channel, value);
        case 0x32:
            return part_update(ControlTarget::tvf_cutoff, channel, value);
        case 0x33:
            return part_update(ControlTarget::tvf_resonance, channel, value);
        case 0x34:
            return part_update(ControlTarget::env_attack, channel, value);
        case 0x35:
            return part_update(ControlTarget::env_decay, channel, value);
        case 0x36:
            return part_update(ControlTarget::env_release, channel, value);
        case 0x37:
            return part_update(ControlTarget::vibrato_delay, channel, value);
        default:
            return std::nullopt;
        }
    }

    // The control matrix. Only the pitch destination of each source is a shared target; the rest is
    // stored whole by the live path, which has somewhere to put it.
    if ((a2 & 0xF0) == 0x20) {
        const int channel = part_of(a2 & 0x0F);
        if (a3 == 0x00) {
            return part_update(ControlTarget::matrix_modulation_pitch, channel, value);
        }
        if (a3 == 0x10) {
            // Bend's pitch depth is the bend range, in the engine and in both front ends.
            return part_update(ControlTarget::bend_range, channel, value - 0x40);
        }
        if (a3 == 0x20) {
            return part_update(ControlTarget::matrix_pressure_pitch, channel, value);
        }
        return std::nullopt;
    }

    // The extended part block.
    if ((a2 & 0xF0) == 0x40 && a3 == 0x20) {
        return part_update(ControlTarget::eq_enabled, part_of(a2 & 0x0F), value);
    }

    return std::nullopt;
}

XgAddress decode_xg_sysex(std::span<const std::uint8_t> bytes) noexcept
{
    // F0 43 1n <model> <aH> <aM> <aL> <data...> F7. There is no checksum in XG.
    if (bytes.size() < 8 || bytes[0] != 0xF0 || bytes[1] != 0x43 || bytes.back() != 0xF7) {
        return {};
    }

    // Device `1n` is a parameter change; `0n` is a bulk dump, which this engine does not accept.
    if ((bytes[2] & 0xF0) != 0x10) {
        return {};
    }

    // The module never checks the model byte once its XG parser is armed, so any Yamaha message
    // with a `1n` device reaches the same address decode. That is worth *not* reproducing: it means
    // a TG300B or MU-native message is read as though it were XG and can write real parameters.
    // Requiring `4C` costs nothing a real XG file needs.
    if (bytes[3] != 0x4C) {
        return {};
    }

    XgAddress address;
    address.high = bytes[4];
    address.mid = bytes[5];
    address.low = bytes[6];
    address.value = bytes.size() > 8 ? bytes[7] : 0;

    if (address.high == 0x00 && address.mid == 0x00) {
        if (address.low == 0x7E) {
            address.kind = XgMessage::system_on;
        } else if (address.low == 0x7F) {
            address.kind = XgMessage::all_parameter_reset;
        } else if (address.low <= 0x06) {
            address.kind = XgMessage::system_parameter;
        }
        return address;
    }

    if (address.high == 0x02 && address.mid == 0x01) {
        address.kind = XgMessage::effect1;
        return address;
    }

    if (address.high == 0x08) {
        address.kind = XgMessage::multi_part;
        // No remap, deliberately. The module keeps its parts in GS block order with channel 10
        // first, so it puts XG's part number through a 64-entry table to get there; this engine
        // indexes parts by MIDI channel, which is already what XG counts. `nn` is the part index,
        // and it stays that way past sixteen for the second and later ports.
        address.part = address.mid;
        return address;
    }

    if (address.high >= 0x30 && address.high <= 0x3F) {
        address.kind = XgMessage::drum_setup;
        return address;
    }

    return address;
}

std::optional<ControlUpdate> decode_xg_multi_part(const XgAddress& address) noexcept
{
    if (address.kind != XgMessage::multi_part || address.part < 0) {
        return std::nullopt;
    }

    const int channel = address.part;
    const int value = address.value;

    switch (address.low) {
    case 0x0B:
        return part_update(ControlTarget::volume, channel, value);
    case 0x0C:
        return part_update(ControlTarget::velocity_depth, channel, value);
    case 0x0D:
        return part_update(ControlTarget::velocity_offset, channel, value);
    // XG pan 0 is "random", the same convention CC#10 has and the same fold applies.
    case 0x0E:
        return part_update(ControlTarget::pan, channel, value == 0 ? 1 : value);
    case 0x12:
        return part_update(ControlTarget::chorus_send, channel, value);
    case 0x13:
        return part_update(ControlTarget::reverb_send, channel, value);
    case 0x15:
        return part_update(ControlTarget::vibrato_rate, channel, value);
    case 0x16:
        return part_update(ControlTarget::vibrato_depth, channel, value);
    case 0x17:
        return part_update(ControlTarget::vibrato_delay, channel, value);
    case 0x18:
        return part_update(ControlTarget::tvf_cutoff, channel, value);
    case 0x19:
        return part_update(ControlTarget::tvf_resonance, channel, value);
    case 0x1A:
        return part_update(ControlTarget::env_attack, channel, value);
    case 0x1B:
        return part_update(ControlTarget::env_decay, channel, value);
    case 0x1C:
        return part_update(ControlTarget::env_release, channel, value);
    default:
        return std::nullopt;
    }
}

} // namespace ts
