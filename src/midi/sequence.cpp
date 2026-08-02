#include "tabulasonora/sequence.hpp"

#include <algorithm>

namespace ts {

// ---------------------------------------------------------------------------------------------
// ControllerTimeline
// ---------------------------------------------------------------------------------------------

int ControllerTimeline::value_at(std::int64_t position, int fallback) const noexcept
{
    int result = fallback;
    for (const Point& point : points_) {
        if (point.position > position) {
            break;
        }
        result = point.value;
    }
    return result;
}

bool ControllerTimeline::fill(std::int64_t start,
                              std::span<int> destination,
                              int fallback) const noexcept
{
    int value = fallback;
    std::size_t cursor = 0;

    // Advance past everything already in force at the start.
    while (cursor < points_.size() && points_[cursor].position <= start) {
        value = points_[cursor].value;
        ++cursor;
    }

    bool changed = false;
    for (std::size_t i = 0; i < destination.size(); ++i) {
        const std::int64_t at = start + static_cast<std::int64_t>(i);
        while (cursor < points_.size() && points_[cursor].position <= at) {
            if (points_[cursor].value != value) {
                changed = true;
            }
            value = points_[cursor].value;
            ++cursor;
        }
        destination[i] = value;
    }

    return changed;
}

// ---------------------------------------------------------------------------------------------
// DrumKeyOverrides
// ---------------------------------------------------------------------------------------------

void DrumKeyOverrides::reset() noexcept
{
    pitch_.fill(0);
    pan_.fill(-1);
    play_key_.fill(-1);
    level_.fill(-1);
    group_.fill(-1);
    reverb_.fill(-1);
    chorus_.fill(-1);
    delay_.fill(-1);
    any_ = false;
}

namespace {

/// Shared plumbing for the drum-setup planes: one value per key, -1 meaning "follow the kit".
void set_plane(std::array<int, DrumKitTable::key_count>& plane, int note, int entry, bool& any)
{
    if (note < 0 || note >= DrumKitTable::key_count) {
        return;
    }
    plane[static_cast<std::size_t>(note)] = entry;
    any = true;
}

[[nodiscard]] std::optional<int> read_plane(const std::array<int, DrumKitTable::key_count>& plane,
                                            int note)
{
    if (note < 0 || note >= DrumKitTable::key_count || plane[static_cast<std::size_t>(note)] < 0) {
        return std::nullopt;
    }
    return plane[static_cast<std::size_t>(note)];
}

} // namespace

void DrumKeyOverrides::set_play_key(int note, int entry) noexcept
{
    set_plane(play_key_, note, entry, any_);
}

void DrumKeyOverrides::set_level(int note, int entry) noexcept
{
    set_plane(level_, note, entry, any_);
}

void DrumKeyOverrides::set_group(int note, int entry) noexcept
{
    set_plane(group_, note, entry, any_);
}

void DrumKeyOverrides::set_reverb(int note, int entry) noexcept
{
    set_plane(reverb_, note, entry, any_);
}

void DrumKeyOverrides::set_chorus(int note, int entry) noexcept
{
    set_plane(chorus_, note, entry, any_);
}

void DrumKeyOverrides::set_delay(int note, int entry) noexcept
{
    set_plane(delay_, note, entry, any_);
}

std::optional<int> DrumKeyOverrides::play_key(int note) const noexcept
{
    return read_plane(play_key_, note);
}

std::optional<int> DrumKeyOverrides::level(int note) const noexcept
{
    return read_plane(level_, note);
}

std::optional<int> DrumKeyOverrides::group(int note) const noexcept
{
    return read_plane(group_, note);
}

void DrumKeyOverrides::set_pitch(int note, int entry) noexcept
{
    if (note < 0 || note >= DrumKitTable::key_count) {
        return;
    }
    pitch_[static_cast<std::size_t>(note)] = entry - centre;
    any_ = true;
}

void DrumKeyOverrides::set_pan(int note, int entry) noexcept
{
    if (note < 0 || note >= DrumKitTable::key_count) {
        return;
    }
    pan_[static_cast<std::size_t>(note)] = entry;
    any_ = true;
}

int DrumKeyOverrides::pitch_offset(int note) const noexcept
{
    return (note >= 0 && note < DrumKitTable::key_count) ? pitch_[static_cast<std::size_t>(note)]
                                                         : 0;
}

std::optional<int> DrumKeyOverrides::pan(int note) const noexcept
{
    if (note < 0 || note >= DrumKitTable::key_count || pan_[static_cast<std::size_t>(note)] < 0) {
        return std::nullopt;
    }
    return pan_[static_cast<std::size_t>(note)];
}

std::optional<int> DrumKeyOverrides::pan_for_hit(int note, EngineNoise& noise) const
{
    const std::optional<int> held = pan(note);
    if (!held) {
        return std::nullopt;
    }
    return *held != random_pan ? *held : noise.next_pan();
}

DrumKey
DrumKeyOverrides::apply(DrumKey key, int pitch_offset_steps, std::optional<int> pan_value) noexcept
{
    // The engine's own plane write (`nrpn_apply` case 0x18, confirmed by live plane reads): the
    // offset lands on the kit plane unscaled, clamped to 0-0x7F. What a step is then worth is the
    // tone's own pitch key-follow -- a 100%-follow tone moves a semitone per step, a 50% tone half
    // of one. The doubling this used to do belonged to that key-follow, not to the plane, and it
    // pushed 100%-follow keys twice as far as the engine does -- WATRWLD1.MID's NRPN-dropped crash
    // (key 55, -40 steps) fell five octaves instead of the engine's 2.3 and vanished on the SC-55
    // map, whose crash tone follows at 100%.
    key.pitch = std::clamp(key.pitch + pitch_offset_steps, 0, 0x7F);
    key.pan = pan_value.value_or(key.pan);
    return key;
}

// ---------------------------------------------------------------------------------------------
// SequenceBuilder
// ---------------------------------------------------------------------------------------------

namespace sequence_builder {
namespace {

/// A note that has sounded and is waiting to close.
struct OpenNote {
    int channel = 0;
    int note = 0;
    std::int64_t on = 0;
    int velocity = 0;
    int drum_pitch = 0;
    std::optional<int> drum_pan;
};

/// A note-off the damper is holding.
struct SustainedNote {
    int channel = 0;
    int note = 0;
    std::int64_t requested_off = 0;
};

/// Everything the builder threads through its event loop.
struct State {
    Sequence sequence;
    // Insertion-ordered rather than a map: the order notes close in is the order they are reported.
    std::vector<OpenNote> open;
    std::vector<SustainedNote> sustained;
    std::array<int, Sequence::channel_count> rpn_msb{};
    std::array<int, Sequence::channel_count> rpn_lsb{};
    // NRPN shares data entry with RPN, so which of the two a CC#6 commits depends on which was
    // selected last. Without this the drum parameters land on the bend range instead.
    std::array<int, Sequence::channel_count> nrpn_msb{};
    std::array<int, Sequence::channel_count> nrpn_lsb{};
    std::array<bool, Sequence::channel_count> data_entry_is_nrpn{};
    std::array<DrumKeyOverrides, Sequence::channel_count> drum_keys;
    // Random pan is resolved while the sequence is built rather than at render time, so this path
    // needs its own generator. It starts from the engine's reset state, which keeps a render of the
    // same file reproducible.
    EngineNoise noise;
    std::int64_t last_position = 0;

    void close_note(int channel, int note, std::int64_t off_position);
};

void State::close_note(int channel, int note, std::int64_t off_position)
{
    const auto slot = std::find_if(open.begin(), open.end(), [&](const OpenNote& o) {
        return o.channel == channel && o.note == note;
    });
    if (slot == open.end()) {
        return;
    }

    const OpenNote held = *slot;
    open.erase(slot);

    const PartTimelines& part = sequence.parts[static_cast<std::size_t>(channel)];
    sequence.notes.push_back(NoteRecord{
        .channel = channel,
        .note = note,
        .velocity = held.velocity,
        .on = held.on,
        .off = off_position,
        .program = part.program.value_at(held.on, 0),
        .bank = part.bank.value_at(held.on, 0),
        .pan = part.pan.value_at(held.on, default_pan),
        .volume = part.volume.value_at(held.on, default_volume),
        .expression = part.expression.value_at(held.on, default_expression),
        .reverb_send = part.reverb_send.value_at(held.on, default_reverb_send),
        .chorus_send = part.chorus_send.value_at(held.on, default_chorus_send),
        .delay_send = part.delay_send.value_at(held.on, 0),
        .drum_pitch = held.drum_pitch,
        .drum_pan = held.drum_pan,
    });
}

void apply_control_change(const MidiEvent& event, int channel, State& state)
{
    PartTimelines& part = state.sequence.parts[static_cast<std::size_t>(channel)];
    const auto slot = static_cast<std::size_t>(channel);
    const int controller = event.data1;
    const int value = event.data2;

    switch (controller) {
    case 1:
        part.modulation.add(event.position, value);
        break;
    case 7:
        part.volume.add(event.position, value);
        break;
    // CC#10 zero is stored as one, so the wheel cannot reach the random position: only the GS
    // SysEx panpot writes a true zero, which is what RND is.
    case 10:
        part.pan.add(event.position, value == 0 ? 1 : value);
        break;
    case 11:
        part.expression.add(event.position, value);
        break;
    case 91:
        part.reverb_send.add(event.position, value);
        break;
    case 93:
        part.chorus_send.add(event.position, value);
        break;

    // Bank select MSB carries the variation; the LSB is unused by this engine.
    case 0:
        part.bank.add(event.position, value);
        break;
    case 32:
        break;

    case 64: {
        part.damper.add(event.position, value);
        if (value < 0x40) {
            // Oldest first, matching the order the reference releases them in -- the note list is
            // ordered by when notes close, so iterating backwards reorders the output.
            std::vector<SustainedNote> releasing;
            for (const SustainedNote& held : state.sustained) {
                if (held.channel == channel) {
                    releasing.push_back(held);
                }
            }
            std::erase_if(state.sustained,
                          [channel](const SustainedNote& s) { return s.channel == channel; });
            for (const SustainedNote& held : releasing) {
                state.close_note(channel, held.note, event.position);
            }
        }
        break;
    }

    case 101:
        state.rpn_msb[slot] = value;
        state.data_entry_is_nrpn[slot] = false;
        break;
    case 100:
        state.rpn_lsb[slot] = value;
        state.data_entry_is_nrpn[slot] = false;
        break;

    case 99:
        state.nrpn_msb[slot] = value;
        state.data_entry_is_nrpn[slot] = true;
        break;
    case 98:
        state.nrpn_lsb[slot] = value;
        state.data_entry_is_nrpn[slot] = true;
        break;

    case 6:
        // Data entry commits whichever of RPN or NRPN was selected last. RPN 00/00 is the bend
        // range in semitones; the NRPNs handled here address one drum key each.
        if (state.data_entry_is_nrpn[slot]) {
            switch (state.nrpn_msb[slot]) {
            case 0x18:
                state.drum_keys[slot].set_pitch(state.nrpn_lsb[slot], value);
                break;
            case 0x1C:
                state.drum_keys[slot].set_pan(state.nrpn_lsb[slot], value);
                break;
            default:
                break;
            }
        } else if (state.rpn_msb[slot] == 0 && state.rpn_lsb[slot] == 0) {
            part.bend_range.add(event.position, value);
        }
        break;

    case 120:
    case 123: {
        std::vector<int> closing;
        for (const OpenNote& held : state.open) {
            if (held.channel == channel) {
                closing.push_back(held.note);
            }
        }
        for (int note : closing) {
            state.close_note(channel, note, event.position);
        }
        std::erase_if(state.sustained,
                      [channel](const SustainedNote& s) { return s.channel == channel; });
        break;
    }

    case 121:
        part.expression.add(event.position, 127);
        part.bend.add(event.position, 8192);
        part.damper.add(event.position, 0);
        part.modulation.add(event.position, 0);
        break;

    default:
        break;
    }
}

void apply_sysex(const MidiEvent& event, State& state)
{
    const std::vector<std::uint8_t>& b = event.sysex;

    // Universal master volume: F0 7F 7F 04 01 ll mm F7.
    if (b.size() >= 8 && b[0] == 0xF0 && b[1] == 0x7F && b[3] == 0x04 && b[4] == 0x01) {
        state.sequence.master_volume.add(event.position, b[6]);
        return;
    }

    // Roland GS DT1: F0 41 dev 42 12 <3-byte address> <data> checksum F7.
    if (b.size() < 11 || b[0] != 0xF0 || b[1] != 0x41 || b[3] != 0x42 || b[4] != 0x12) {
        return;
    }

    const int a1 = b[5];
    const int a2 = b[6];
    const int a3 = b[7];
    const int value = b[8];

    if (a1 == 0x40 && a2 == 0x01) {
        if ((a3 == 0x30 || a3 == 0x31) && value <= 7) {
            state.sequence.reverb_type.add(event.position, value);
        } else if (a3 == 0x38 && value <= 7) {
            state.sequence.chorus_type.add(event.position, value);
        } else if (a3 == 0x50 && value <= 9) {
            state.sequence.delay_type.add(event.position, value);
        }
    } else if (a1 == 0x40 && (a2 & 0xF0) == 0x10 && a3 == 0x2C) {
        state.sequence.parts[static_cast<std::size_t>(channel_from_block(a2 & 0x0F))]
            .delay_send.add(event.position, value);
    }
}

} // namespace

Sequence build(std::span<const MidiEvent> events)
{
    State state;
    state.rpn_msb.fill(0x7F);
    state.rpn_lsb.fill(0x7F);
    state.nrpn_msb.fill(0x7F);
    state.nrpn_lsb.fill(0x7F);
    state.data_entry_is_nrpn.fill(false);

    for (PartTimelines& part : state.sequence.parts) {
        part.bend_range.add(0, 2);
    }

    for (const MidiEvent& event : events) {
        state.last_position = std::max(state.last_position, event.position);

        if (event.kind == MidiEventKind::sysex) {
            apply_sysex(event, state);
            continue;
        }

        const int channel = event.channel();
        PartTimelines& part = state.sequence.parts[static_cast<std::size_t>(channel)];

        switch (event.message_type()) {
        case 0x90:
            if (event.data2 > 0) {
                // Re-striking a still-open note closes the old voice first.
                state.close_note(channel, event.data1, event.position);

                // The new strike supersedes any note-off of this note that the pedal is still
                // holding. Leaving that parked entry in place makes the pedal's lift close the note
                // being struck here, which the player has not released.
                std::erase_if(state.sustained, [&](const SustainedNote& s) {
                    return s.channel == channel && s.note == event.data1;
                });

                // Drum key overrides are latched here rather than read at note-off: they live in a
                // mutable per-part table, not a timeline, so a later NRPN would otherwise reach
                // back and change a note that had already sounded.
                DrumKeyOverrides& overrides = state.drum_keys[static_cast<std::size_t>(channel)];
                state.open.push_back(OpenNote{channel,
                                              event.data1,
                                              event.position,
                                              event.data2,
                                              overrides.pitch_offset(event.data1),
                                              overrides.pan_for_hit(event.data1, state.noise)});
                break;
            }
            [[fallthrough]];

        case 0x80:
            if (part.damper.value_at(event.position, 0) >= 0x40) {
                // Damper down: the release waits for the pedal to lift.
                state.sustained.push_back(SustainedNote{channel, event.data1, event.position});
            } else {
                state.close_note(channel, event.data1, event.position);
            }
            break;

        case 0xC0:
            part.program.add(event.position, event.data1);
            break;

        case 0xE0:
            part.bend.add(event.position, event.data1 | (event.data2 << 7));
            break;

        case 0xB0:
            apply_control_change(event, channel, state);
            break;

        default:
            break;
        }
    }

    // Anything still ringing at the end closes there.
    for (const SustainedNote& held : state.sustained) {
        state.close_note(held.channel, held.note, held.requested_off);
    }

    while (!state.open.empty()) {
        state.close_note(state.open.front().channel, state.open.front().note, state.last_position);
    }

    state.sequence.last_event_position = state.last_position;
    return std::move(state.sequence);
}

} // namespace sequence_builder
} // namespace ts
