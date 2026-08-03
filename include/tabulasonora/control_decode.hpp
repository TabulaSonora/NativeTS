#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace ts {

/// Every part parameter a MIDI message can set to a value.
///
/// This is the vocabulary the two front ends share. It deliberately covers only the messages whose
/// whole effect is "store this number against this part" — the ones where both paths were writing
/// the same knowledge into different files. Messages that *do* something (note on and off, the
/// pedals' release behaviour, the channel-mode messages, program change and its bank resolution)
/// stay with the front end that has the machinery to act on them, because there is nothing to share
/// there but a switch label.
enum class ControlTarget {
    volume,
    expression,
    pan,
    modulation,
    damper,
    reverb_send,
    chorus_send,
    delay_send,
    bank,
    bend_range,

    // The part modify offsets. TVF resonance is absent because the engine never reads it.
    vibrato_rate,
    vibrato_depth,
    vibrato_delay,
    tvf_cutoff,
    env_attack,
    env_decay,
    env_release,

    velocity_depth,
    velocity_offset,

    channel_pressure,

    /// The control matrix's pitch routes, one per source.
    matrix_modulation_pitch,
    matrix_pressure_pitch,

    /// The system EQ block, which is global rather than per part.
    eq_low_frequency,
    eq_low_gain,
    eq_high_frequency,
    eq_high_gain,

    /// Per-part EQ enable (`40 4x 20`).
    eq_enabled,
};

/// One parameter set by one message.
struct ControlUpdate {
    ControlTarget target = ControlTarget::volume;
    /// The part's channel, or -1 for the global targets.
    int channel = -1;
    int value = 0;

    [[nodiscard]] constexpr bool is_global() const noexcept { return channel < 0; }
};

/// Which parameter a Control Change writes, if it writes one.
///
/// CC#10's zero is folded to one here rather than at each call site: the wheel cannot reach the
/// random pan position, and only the GS SysEx panpot can write a true zero.
[[nodiscard]] std::optional<ControlUpdate>
decode_control_change(int channel, int controller, int value) noexcept;

/// Which parameter a Roland GS DT1 message writes, if it writes one.
///
/// Takes the whole message including `F0` and the checksum. Returns nothing for a message that is
/// malformed, fails its checksum, or addresses something outside this vocabulary — a caller that
/// needs those cases handles them itself, and both do.
[[nodiscard]] std::optional<ControlUpdate> decode_gs_sysex(std::span<const std::uint8_t> bytes,
                                                           int port = 0) noexcept;

/// Whether a GS DT1 message is well formed and its checksum folds to zero.
///
/// Exposed because both front ends drop a bad message and neither should be spelling the fold out
/// for itself.
[[nodiscard]] bool gs_checksum_ok(std::span<const std::uint8_t> bytes) noexcept;

} // namespace ts
