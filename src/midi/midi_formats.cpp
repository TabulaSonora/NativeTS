#include "tabulasonora/midi_formats.hpp"

// Foreign-format converters, ported from spessasynth_core_c (src/midi/parsers/), which itself
// ports midi_processing. Each converter reads the foreign container and serialises an equivalent
// Standard MIDI File, so `smf::parse` stays the only reader of events. The C originals build an
// in-memory track structure their sequencer consumes directly; serialising instead costs one
// extra parse and buys keeping every downstream consumer -- tempo map, port scheme selection,
// loop scanning, EMIDI filtering -- in exactly one place.

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(TS_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace ts::formats {
namespace {

using Bytes = std::span<const std::uint8_t>;

// ── Byte accessors ────────────────────────────────────────────────────────────────────────────
//
// Lenient the way the reference port's SS_File is: a read past the end yields zero. Structural
// decisions still check sizes explicitly; the leniency only means a truncated field reads as
// zeros instead of walking off the buffer.

[[nodiscard]] std::uint8_t u8(Bytes data, std::size_t at)
{
    return at < data.size() ? data[at] : 0;
}

[[nodiscard]] std::uint16_t le16(Bytes data, std::size_t at)
{
    return static_cast<std::uint16_t>(u8(data, at) | (u8(data, at + 1) << 8));
}

[[nodiscard]] std::uint32_t le32(Bytes data, std::size_t at)
{
    return static_cast<std::uint32_t>(u8(data, at))
           | (static_cast<std::uint32_t>(u8(data, at + 1)) << 8)
           | (static_cast<std::uint32_t>(u8(data, at + 2)) << 16)
           | (static_cast<std::uint32_t>(u8(data, at + 3)) << 24);
}

[[nodiscard]] std::uint32_t be32(Bytes data, std::size_t at)
{
    return (static_cast<std::uint32_t>(u8(data, at)) << 24)
           | (static_cast<std::uint32_t>(u8(data, at + 1)) << 16)
           | (static_cast<std::uint32_t>(u8(data, at + 2)) << 8)
           | static_cast<std::uint32_t>(u8(data, at + 3));
}

[[nodiscard]] bool tag_at(Bytes data, std::size_t at, const char* tag)
{
    if (at + 4 > data.size()) {
        return false;
    }
    return std::memcmp(data.data() + at, tag, 4) == 0;
}

/// Standard MIDI variable-length quantity at `pos`, advancing it.
[[nodiscard]] std::uint32_t read_vlq(Bytes data, std::size_t& pos)
{
    std::uint32_t value = 0;
    while (pos < data.size()) {
        const std::uint8_t byte = data[pos];
        ++pos;
        value = (value << 7) | (byte & 0x7FU);
        if ((byte & 0x80U) == 0) {
            break;
        }
    }
    return value;
}

[[noreturn]] void malformed(const char* format)
{
    throw std::runtime_error(std::string{"Malformed "} + format + " file.");
}

// ── Tick timeline and SMF serialisation ───────────────────────────────────────────────────────

constexpr std::uint8_t meta_text = 0x01;
constexpr std::uint8_t meta_marker = 0x06;
constexpr std::uint8_t meta_end_of_track = 0x2F;
constexpr std::uint8_t meta_set_tempo = 0x51;

/// One event on a tick timeline, before serialisation.
struct TickEvent {
    std::int64_t tick = 0;
    /// `0xFF` for a meta event (type in `meta`), `0xF0` for SysEx (payload without the leading
    /// F0), otherwise the channel-voice status byte.
    std::uint8_t status = 0;
    std::uint8_t meta = 0;
    std::vector<std::uint8_t> data;
};

using TickTrack = std::vector<TickEvent>;

void push_meta(TickTrack& track, std::int64_t tick, std::uint8_t type, Bytes payload)
{
    track.push_back(TickEvent{tick, 0xFF, type, {payload.begin(), payload.end()}});
}

void push_sysex(TickTrack& track, std::int64_t tick, Bytes payload)
{
    track.push_back(TickEvent{tick, 0xF0, 0, {payload.begin(), payload.end()}});
}

void push_voice(TickTrack& track, std::int64_t tick, std::uint8_t status, std::uint8_t d1)
{
    track.push_back(TickEvent{tick, status, 0, {d1}});
}

void push_voice(TickTrack& track,
                std::int64_t tick,
                std::uint8_t status,
                std::uint8_t d1,
                std::uint8_t d2)
{
    track.push_back(TickEvent{tick, status, 0, {d1, d2}});
}

void write_vlq(std::vector<std::uint8_t>& out, std::int64_t value)
{
    const auto quantity = static_cast<std::uint64_t>(std::max<std::int64_t>(0, value));
    std::array<std::uint8_t, 10> groups{};
    std::size_t count = 0;
    std::uint64_t rest = quantity;
    do {
        groups[count++] = static_cast<std::uint8_t>(rest & 0x7FU);
        rest >>= 7;
    } while (rest != 0);
    while (count > 1) {
        out.push_back(static_cast<std::uint8_t>(groups[--count] | 0x80U));
    }
    out.push_back(groups[0]);
}

void write_be16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void write_be32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

/// Serialises one tick track as an MTrk chunk.
///
/// Events are stable-sorted by tick first: converters push synthesised note-offs (XMI and HMI
/// store note durations, not note-offs) at their future tick the moment the note-on is read, so
/// the timeline arrives out of order. The stable sort reproduces the C port's sorted-insert --
/// order of arrival breaks ties. Voice data bytes are masked to seven bits: the C feeds its
/// sequencer structures directly and can carry an out-of-range byte harmlessly, but here a data
/// byte with the high bit set would desync the SMF parse of everything after it.
void append_track(std::vector<std::uint8_t>& out, TickTrack track)
{
    std::stable_sort(track.begin(), track.end(), [](const TickEvent& a, const TickEvent& b) {
        return a.tick < b.tick;
    });

    if (track.empty() || track.back().status != 0xFF
        || track.back().meta != meta_end_of_track) {
        const std::int64_t tick = track.empty() ? 0 : track.back().tick;
        push_meta(track, tick, meta_end_of_track, {});
    }

    std::vector<std::uint8_t> body;
    std::int64_t previous = 0;
    for (const TickEvent& event : track) {
        write_vlq(body, event.tick - previous);
        previous = event.tick;

        if (event.status == 0xFF) {
            body.push_back(0xFF);
            body.push_back(event.meta);
            write_vlq(body, static_cast<std::int64_t>(event.data.size()));
            body.insert(body.end(), event.data.begin(), event.data.end());
        } else if (event.status == 0xF0) {
            body.push_back(0xF0);
            write_vlq(body, static_cast<std::int64_t>(event.data.size()));
            body.insert(body.end(), event.data.begin(), event.data.end());
        } else {
            body.push_back(event.status);
            for (const std::uint8_t byte : event.data) {
                body.push_back(static_cast<std::uint8_t>(byte & 0x7FU));
            }
        }
    }

    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    write_be32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void append_header(std::vector<std::uint8_t>& out,
                   std::uint16_t format,
                   std::uint16_t track_count,
                   std::uint16_t division)
{
    out.insert(out.end(), {'M', 'T', 'h', 'd'});
    write_be32(out, 6);
    write_be16(out, format);
    write_be16(out, track_count);
    write_be16(out, division);
}

[[nodiscard]] std::vector<std::uint8_t>
write_smf(std::uint16_t format, std::uint16_t division, std::vector<TickTrack> tracks)
{
    std::vector<std::uint8_t> out;
    append_header(out, format, static_cast<std::uint16_t>(tracks.size()), division);
    for (TickTrack& track : tracks) {
        append_track(out, std::move(track));
    }
    return out;
}

// ── RIFF-MIDI (RMID) ──────────────────────────────────────────────────────────────────────────
//
// A RIFF wrapper whose first chunk is the raw SMF bytes. The reference also extracts an embedded
// SF2/DLS soundbank and LIST/INFO metadata; this engine renders from its own ROM and consumes
// neither, so unwrapping is the whole job.

[[nodiscard]] std::vector<std::uint8_t> convert_rmidi(Bytes data)
{
    if (data.size() < 20 || !tag_at(data, 8, "RMID")) {
        malformed("RIFF-MIDI");
    }
    const std::size_t riff_size = le32(data, 4);
    if (riff_size + 8 > data.size() || !tag_at(data, 12, "data")) {
        malformed("RIFF-MIDI");
    }
    const std::size_t smf_size = le32(data, 16);
    const std::size_t start = 20;
    const std::size_t take = std::min(smf_size, data.size() - start);
    return {data.begin() + static_cast<std::ptrdiff_t>(start),
            data.begin() + static_cast<std::ptrdiff_t>(start + take)};
}

// ── Microsoft DirectMusic Segment (MIDS) ──────────────────────────────────────────────────────

[[nodiscard]] bool is_mids(Bytes data)
{
    if (data.size() < 16 || !tag_at(data, 0, "RIFF")) {
        return false;
    }
    const std::uint32_t riff_size = le32(data, 4);
    if (riff_size < 8 || data.size() < static_cast<std::size_t>(riff_size) + 8) {
        return false;
    }
    return tag_at(data, 8, "MIDS") && tag_at(data, 12, "fmt ");
}

[[nodiscard]] std::vector<std::uint8_t> convert_mids(Bytes data)
{
    if (data.size() < 20) {
        malformed("MIDS");
    }

    std::size_t pos = 16;
    std::uint32_t fmt_size = le32(data, pos);
    pos += 4;
    if (data.size() - pos < fmt_size) {
        malformed("MIDS");
    }

    std::uint32_t time_division = 1;
    std::uint32_t flags = 0;
    const std::size_t fmt_end = pos + fmt_size;

    if (fmt_size >= 4) {
        time_division = le32(data, pos);
        pos += 4;
        fmt_size -= 4;
        if (time_division == 0) {
            malformed("MIDS");
        }
    }
    if (fmt_size >= 4) {
        pos += 4; // max_buffer -- unused
        fmt_size -= 4;
    }
    if (fmt_size >= 4) {
        flags = le32(data, pos);
        pos += 4;
    }

    pos = fmt_end;
    if (pos >= data.size()) {
        malformed("MIDS");
    }
    if (fmt_end & 1) {
        ++pos; // RIFF pads to even
    }

    if (data.size() - pos < 4 || !tag_at(data, pos, "data")) {
        malformed("MIDS");
    }
    pos += 4;

    TickTrack track;

    if (data.size() - pos < 4) {
        malformed("MIDS");
    }
    const std::uint32_t data_size = le32(data, pos);
    pos += 4;
    std::size_t body_end = pos + data_size;
    if (body_end > data.size()) {
        body_end = data.size();
    }

    if (body_end - pos < 4) {
        malformed("MIDS");
    }
    const std::uint32_t segment_count = le32(data, pos);
    pos += 4;

    const bool is_eight_byte = (flags & 1U) != 0;
    std::int64_t current_ticks = 0;

    for (std::uint32_t i = 0; i < segment_count; ++i) {
        if (data.size() - pos < 12) {
            malformed("MIDS");
        }
        pos += 4; // unused segment header word
        const std::uint32_t segment_size = le32(data, pos);
        pos += 4;
        std::size_t segment_end = pos + segment_size;
        if (segment_end > body_end) {
            segment_end = body_end;
        }

        while (pos < segment_end) {
            if (segment_end - pos < 4) {
                malformed("MIDS");
            }
            current_ticks += le32(data, pos);
            pos += 4;

            if (!is_eight_byte) {
                if (segment_end - pos < 4) {
                    malformed("MIDS");
                }
                pos += 4; // unused extra word
            }

            if (segment_end - pos < 4) {
                malformed("MIDS");
            }
            const std::uint32_t event = le32(data, pos);
            pos += 4;

            const auto record = static_cast<std::uint8_t>(event >> 24);
            if (record == 0x01) {
                // Tempo record: low 24 bits are microseconds per beat.
                const std::array<std::uint8_t, 3> tempo{static_cast<std::uint8_t>(event >> 16),
                                                        static_cast<std::uint8_t>(event >> 8),
                                                        static_cast<std::uint8_t>(event)};
                push_meta(track, current_ticks, meta_set_tempo, tempo);
            } else if (record == 0x00) {
                // Voice message packed into the 32-bit word.
                const auto status = static_cast<std::uint8_t>(event & 0xFFU);
                const auto high_nibble = static_cast<std::uint8_t>((status & 0xF0U) >> 4);
                if (high_nibble < 0x8 || high_nibble > 0xE) {
                    continue;
                }
                const auto d1 = static_cast<std::uint8_t>((event >> 8) & 0xFFU);
                if (high_nibble == 0xC || high_nibble == 0xD) {
                    push_voice(track, current_ticks, status, d1);
                } else {
                    push_voice(track,
                               current_ticks,
                               status,
                               d1,
                               static_cast<std::uint8_t>((event >> 16) & 0xFFU));
                }
            }
            // Other record tags are silently ignored, matching the reference.
        }
    }

    std::vector<TickTrack> tracks;
    tracks.push_back(std::move(track));
    return write_smf(0, static_cast<std::uint16_t>(time_division), std::move(tracks));
}

// ── DOOM / Heretic MUS ────────────────────────────────────────────────────────────────────────

/// MUS controller index to MIDI controller number: 0..9 are ordinary controllers (index 0 is the
/// program-change path and unused here), 10..14 are the mode-change controllers.
constexpr std::array<std::uint8_t, 15> mus_controllers{0,  0,  1,   7,   10, 11, 91, 93,
                                                       64, 67, 120, 123, 126, 127, 121};

/// Default MUS tempo, roughly 95 BPM at TPQN 89, approximating DOOM's 140 Hz playback rate.
constexpr std::array<std::uint8_t, 3> mus_default_tempo{0x09, 0xA3, 0x1A};
constexpr std::uint16_t mus_time_division = 89;

[[nodiscard]] bool is_mus(Bytes data)
{
    if (data.size() < 0x20) {
        return false;
    }
    if (u8(data, 0) != 'M' || u8(data, 1) != 'U' || u8(data, 2) != 'S' || u8(data, 3) != 0x1A) {
        return false;
    }
    const std::uint16_t length = le16(data, 4);
    const std::uint16_t offset = le16(data, 6);
    const std::uint16_t instrument_count = le16(data, 12);

    // The offset must point past the instrument list, and the score must fit in the file.
    const std::size_t min_off = 16 + static_cast<std::size_t>(instrument_count) * 2;
    const std::size_t max_off = 16 + static_cast<std::size_t>(instrument_count) * 4;
    if (offset < min_off || offset >= max_off) {
        return false;
    }
    return static_cast<std::size_t>(offset) + length <= data.size();
}

[[nodiscard]] std::vector<std::uint8_t> convert_mus(Bytes data)
{
    const std::uint16_t length = le16(data, 4);
    const std::uint16_t offset = le16(data, 6);

    TickTrack track;
    push_meta(track, 0, meta_set_tempo, mus_default_tempo);

    // Slice the event stream so reads cannot overrun into trailing data.
    const Bytes score = data.subspan(offset, length);
    std::size_t pos = 0;
    std::int64_t current_ticks = 0;
    std::array<std::uint8_t, 16> velocity_levels{};

    while (pos < score.size()) {
        const std::uint8_t event = score[pos];
        ++pos;
        if (event == 0x60) {
            break; // score end marker
        }

        std::uint8_t channel = event & 0x0FU;
        // The MUS drum channel (15) becomes MIDI 9; the MIDI 9 slot is shifted up.
        if (channel == 0x0F) {
            channel = 9;
        } else if (channel >= 9) {
            ++channel;
        }

        switch (event & 0x70U) {
        case 0x00: { // note release: note-on with velocity zero
            if (pos >= score.size()) {
                malformed("MUS");
            }
            push_voice(track,
                       current_ticks,
                       static_cast<std::uint8_t>(0x90U | channel),
                       score[pos],
                       0);
            ++pos;
            break;
        }

        case 0x10: { // note play
            if (pos >= score.size()) {
                malformed("MUS");
            }
            std::uint8_t note = score[pos];
            ++pos;
            std::uint8_t velocity = 0;
            if ((note & 0x80U) != 0) {
                if (pos >= score.size()) {
                    malformed("MUS");
                }
                velocity = score[pos];
                ++pos;
                velocity_levels[channel] = velocity;
                note &= 0x7FU;
            } else {
                velocity = velocity_levels[channel];
            }
            push_voice(track,
                       current_ticks,
                       static_cast<std::uint8_t>(0x90U | channel),
                       note,
                       velocity);
            break;
        }

        case 0x20: { // pitch wheel: 8-bit MUS to 14-bit MIDI, 0x80 maps to centre
            if (pos >= score.size()) {
                malformed("MUS");
            }
            const std::uint8_t wheel = score[pos];
            ++pos;
            push_voice(track,
                       current_ticks,
                       static_cast<std::uint8_t>(0xE0U | channel),
                       static_cast<std::uint8_t>((static_cast<unsigned>(wheel) << 6) & 0x7FU),
                       static_cast<std::uint8_t>(wheel >> 1));
            break;
        }

        case 0x30: { // system/mode-change controller (indexes 10..14)
            if (pos >= score.size()) {
                malformed("MUS");
            }
            const std::uint8_t index = score[pos];
            ++pos;
            if (index < 10 || index > 14) {
                malformed("MUS");
            }
            push_voice(track,
                       current_ticks,
                       static_cast<std::uint8_t>(0xB0U | channel),
                       mus_controllers[index],
                       1);
            break;
        }

        case 0x40: { // controller change (index 1..9) or program change (index 0)
            if (pos >= score.size()) {
                malformed("MUS");
            }
            const std::uint8_t index = score[pos];
            ++pos;
            if (index == 0) {
                if (pos >= score.size()) {
                    malformed("MUS");
                }
                push_voice(track,
                           current_ticks,
                           static_cast<std::uint8_t>(0xC0U | channel),
                           score[pos]);
                ++pos;
            } else if (index < 10) {
                if (pos >= score.size()) {
                    malformed("MUS");
                }
                push_voice(track,
                           current_ticks,
                           static_cast<std::uint8_t>(0xB0U | channel),
                           mus_controllers[index],
                           score[pos]);
                ++pos;
            } else {
                malformed("MUS");
            }
            break;
        }

        default:
            malformed("MUS");
        }

        // A delta time follows when bit 7 of the event byte is set.
        if ((event & 0x80U) != 0) {
            const std::size_t before = pos;
            current_ticks += read_vlq(score, pos);
            if (pos <= before) {
                malformed("MUS"); // the quantity made no progress
            }
        }
    }

    std::vector<TickTrack> tracks;
    tracks.push_back(std::move(track));
    return write_smf(0, mus_time_division, std::move(tracks));
}

// ── Miles Sound System XMI ────────────────────────────────────────────────────────────────────

constexpr std::array<std::uint8_t, 3> xmi_default_tempo{0x07, 0xA1, 0x20}; // 500000 us/beat

[[nodiscard]] bool is_xmi(Bytes data)
{
    if (data.size() < 0x22) {
        return false;
    }
    return tag_at(data, 0, "FORM") && tag_at(data, 8, "XDIR") && tag_at(data, 0x1E, "XMID");
}

/// Parses one EVNT body into a track starting at `start_ticks`, returning the tail tick.
[[nodiscard]] std::int64_t parse_xmi_track(TickTrack& track,
                                           Bytes events,
                                           std::int64_t start_ticks)
{
    std::size_t pos = 0;
    std::int64_t current_ticks = start_ticks;
    std::int64_t last_event_ticks = start_ticks;
    bool initial_tempo_set = false;

    while (pos < events.size()) {
        // Delta: the sum of bytes below 0x80 until a status byte appears.
        std::int64_t delta = 0;
        while (pos < events.size()) {
            const std::uint8_t byte = events[pos];
            if ((byte & 0x80U) != 0) {
                break;
            }
            delta += byte;
            ++pos;
        }
        current_ticks += delta;
        last_event_ticks = std::max(last_event_ticks, current_ticks);

        if (pos >= events.size()) {
            break;
        }
        const std::uint8_t status = events[pos];
        ++pos;

        if (status == 0xFF) {
            if (pos >= events.size()) {
                malformed("XMI");
            }
            const std::uint8_t meta_type = events[pos];
            ++pos;
            std::size_t meta_length = 0;
            if (meta_type != meta_end_of_track) {
                meta_length = read_vlq(events, pos);
                if (events.size() - pos < meta_length) {
                    malformed("XMI");
                }
            }

            std::vector<std::uint8_t> payload{events.begin() + static_cast<std::ptrdiff_t>(pos),
                                              events.begin()
                                                  + static_cast<std::ptrdiff_t>(pos + meta_length)};
            pos += meta_length;

            // XMI tempo normalisation: the container runs a fixed 120 Hz clock at 60 TPQN, and
            // this rescale maps the stored value onto a standard tempo meta event.
            if (meta_type == meta_set_tempo && payload.size() == 3) {
                const std::uint32_t stored = (static_cast<std::uint32_t>(payload[0]) << 16)
                                             | (static_cast<std::uint32_t>(payload[1]) << 8)
                                             | payload[2];
                const std::uint32_t ppqn = (stored * 3U) / 25000U;
                if (ppqn > 0) {
                    const std::uint32_t rescaled = (stored * 60U) / ppqn;
                    payload[0] = static_cast<std::uint8_t>(rescaled >> 16);
                    payload[1] = static_cast<std::uint8_t>((rescaled >> 8) & 0xFFU);
                    payload[2] = static_cast<std::uint8_t>(rescaled & 0xFFU);
                }
                if (current_ticks == start_ticks) {
                    initial_tempo_set = true;
                }
            }

            // End of Track slides forward to cover pending synthesised note-offs.
            if (meta_type == meta_end_of_track && current_ticks < last_event_ticks) {
                current_ticks = last_event_ticks;
            }

            push_meta(track, current_ticks, meta_type, payload);
            if (meta_type == meta_end_of_track) {
                break;
            }

        } else if (status == 0xF0) {
            const std::size_t sysex_length = read_vlq(events, pos);
            if (events.size() - pos < sysex_length) {
                malformed("XMI");
            }
            push_sysex(track, current_ticks, events.subspan(pos, sysex_length));
            pos += sysex_length;

        } else if (status >= 0x80 && status <= 0xEF) {
            const auto type_nibble = static_cast<std::uint8_t>(status >> 4);
            if (pos >= events.size()) {
                malformed("XMI");
            }
            const std::uint8_t d1 = events[pos];
            ++pos;
            if (type_nibble == 0xC || type_nibble == 0xD) {
                push_voice(track, current_ticks, status, d1);
            } else {
                if (pos >= events.size()) {
                    malformed("XMI");
                }
                push_voice(track, current_ticks, status, d1, events[pos]);
                ++pos;
            }

            // Note-on stores a duration; synthesise the matching note-off.
            if (type_nibble == 0x9) {
                const std::int64_t duration = read_vlq(events, pos);
                const std::int64_t note_end = current_ticks + duration;
                last_event_ticks = std::max(last_event_ticks, note_end);
                push_voice(track, note_end, status, d1, 0); // velocity 0 is a note-off
            }

        } else {
            malformed("XMI");
        }
    }

    // Seed the default tempo at track start when the source did not.
    if (!initial_tempo_set) {
        push_meta(track, start_ticks, meta_set_tempo, xmi_default_tempo);
    }

    return last_event_ticks;
}

[[nodiscard]] std::vector<std::uint8_t> convert_xmi(Bytes data)
{
    // Top-level FORM XDIR: skip entirely.
    std::size_t pos = 0;
    if (!tag_at(data, pos, "FORM")) {
        malformed("XMI");
    }
    std::uint32_t size = be32(data, pos + 4);
    pos += 8;
    std::size_t form_end = pos + size;
    if (form_end > data.size()) {
        form_end = data.size();
    }
    pos = form_end;
    if ((size & 1U) != 0 && pos < data.size()) {
        ++pos;
    }

    // Top-level CAT XMID.
    if (!tag_at(data, pos, "CAT ")) {
        malformed("XMI");
    }
    size = be32(data, pos + 4);
    pos += 8;
    std::size_t cat_end = pos + size;
    if (cat_end > data.size()) {
        cat_end = data.size();
    }
    if (cat_end - pos < 4 || !tag_at(data, pos, "XMID")) {
        malformed("XMI");
    }
    pos += 4;

    // Each FORM XMID within the CAT is one track; find its EVNT chunk and parse it. The loader
    // convention for multi-track collections concatenates the tick timelines, so each track is
    // offset by the previous track's tail tick.
    std::vector<TickTrack> tracks;
    std::int64_t start_ticks = 0;
    std::size_t scan = pos;
    while (scan + 8 <= cat_end) {
        const std::uint32_t chunk_size = be32(data, scan + 4);
        const bool is_form = tag_at(data, scan, "FORM");
        const std::size_t body = scan + 8;
        std::size_t body_end = body + chunk_size;
        if (body_end > cat_end) {
            body_end = cat_end;
        }
        const std::size_t next = body_end + ((chunk_size & 1U) != 0 ? 1 : 0);

        if (!is_form) {
            scan = next;
            continue;
        }
        if (body_end - body < 4 || !tag_at(data, body, "XMID")) {
            malformed("XMI");
        }

        std::size_t sub = body + 4;
        std::size_t event_start = 0;
        std::uint32_t event_size = 0;
        while (sub + 8 <= body_end) {
            std::uint32_t sub_size = be32(data, sub + 4);
            const bool is_evnt = tag_at(data, sub, "EVNT");
            sub += 8;
            if (sub_size > body_end - sub) {
                sub_size = static_cast<std::uint32_t>(body_end - sub);
            }
            if (is_evnt) {
                event_start = sub;
                event_size = sub_size;
                break;
            }
            sub += sub_size;
            if ((sub_size & 1U) != 0 && sub < body_end) {
                ++sub;
            }
        }
        if (event_start == 0) {
            malformed("XMI");
        }

        TickTrack track;
        start_ticks = parse_xmi_track(track, data.subspan(event_start, event_size), start_ticks);
        tracks.push_back(std::move(track));

        scan = next;
    }

    if (tracks.empty()) {
        malformed("XMI");
    }
    const std::uint16_t format = tracks.size() > 1 ? 2 : 0;
    return write_smf(format, 60, std::move(tracks));
}

// ── General MIDI Format (GMF) ─────────────────────────────────────────────────────────────────

[[nodiscard]] bool is_gmf(Bytes data)
{
    if (data.size() < 32) {
        return false;
    }
    return u8(data, 0) == 'G' && u8(data, 1) == 'M' && u8(data, 2) == 'F' && u8(data, 3) == 1;
}

[[nodiscard]] std::vector<std::uint8_t> convert_gmf(Bytes data)
{
    const std::uint32_t tempo_raw =
        (static_cast<std::uint32_t>(u8(data, 4)) << 8) | u8(data, 5);
    const std::uint32_t tempo_us = tempo_raw * 100000U;

    // Track 0 -- conductor: tempo meta and a Roland GS-style reset SysEx, kept verbatim from the
    // reference port.
    TickTrack conductor;
    const std::array<std::uint8_t, 3> tempo{static_cast<std::uint8_t>(tempo_us >> 16),
                                            static_cast<std::uint8_t>((tempo_us >> 8) & 0xFFU),
                                            static_cast<std::uint8_t>(tempo_us & 0xFFU)};
    push_meta(conductor, 0, meta_set_tempo, tempo);
    constexpr std::array<std::uint8_t, 9> reset{0x41, 0x10, 0x16, 0x12, 0x7F,
                                                0x00, 0x00, 0x01, 0xF7};
    push_sysex(conductor, 0, reset);

    // Track 1 is already a raw MTrk body -- VLQ deltas, running status -- so it is embedded
    // verbatim rather than decoded and re-encoded.
    std::vector<std::uint8_t> out;
    append_header(out, 0, 2, 0xC0);
    append_track(out, std::move(conductor));
    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    write_be32(out, static_cast<std::uint32_t>(data.size() - 7));
    out.insert(out.end(), data.begin() + 7, data.end());
    return out;
}

// ── HMI Sound Operating System (HMP) ──────────────────────────────────────────────────────────

/// Default conductor tempo shared by both HMI containers: 0x188000 us/beat at TPQN 0xC0, which is
/// about 120 ticks per second.
constexpr std::array<std::uint8_t, 3> hmi_default_tempo{0x18, 0x80, 0x00};

[[nodiscard]] bool is_hmp(Bytes data)
{
    if (data.size() < 8) {
        return false;
    }
    static constexpr char magic[] = "HMIMIDI";
    for (std::size_t i = 0; i < 7; ++i) {
        if (u8(data, i) != static_cast<std::uint8_t>(magic[i])) {
            return false;
        }
    }
    const std::uint8_t last = u8(data, 7);
    return last == 'P' || last == 'R';
}

/// HMP delta: little-endian seven-bit groups where a byte with the high bit *set* terminates --
/// the inverse of the MIDI quantity.
[[nodiscard]] std::int64_t read_hmp_delta(Bytes data, std::size_t& pos)
{
    std::int64_t delta = 0;
    unsigned shift = 0;
    while (pos < data.size()) {
        const std::uint8_t byte = data[pos];
        ++pos;
        delta += static_cast<std::int64_t>(byte & 0x7FU) << shift;
        shift += 7;
        if ((byte & 0x80U) != 0) {
            break;
        }
    }
    return delta;
}

void parse_hmp_track(Bytes body, TickTrack& track)
{
    std::size_t pos = 0;
    std::int64_t tick = 0;

    while (pos < body.size()) {
        tick += read_hmp_delta(body, pos);
        if (pos >= body.size()) {
            malformed("HMP");
        }

        const std::uint8_t status = body[pos];
        ++pos;

        if (status == 0xFF) {
            if (pos >= body.size()) {
                malformed("HMP");
            }
            const std::uint8_t meta_type = body[pos];
            ++pos;
            const std::size_t meta_length = read_vlq(body, pos);
            if (body.size() - pos < meta_length) {
                malformed("HMP");
            }
            push_meta(track, tick, meta_type, body.subspan(pos, meta_length));
            pos += meta_length;
            if (meta_type == meta_end_of_track) {
                break;
            }
        } else if (status >= 0x80 && status <= 0xEF) {
            // Explicit status on every event: HMP has no running status and no SysEx.
            const std::uint8_t type = status & 0xF0U;
            if (type == 0xC0 || type == 0xD0) {
                if (body.size() - pos < 1) {
                    malformed("HMP");
                }
                push_voice(track, tick, status, body[pos]);
                pos += 1;
            } else {
                if (body.size() - pos < 2) {
                    malformed("HMP");
                }
                push_voice(track, tick, status, body[pos], body[pos + 1]);
                pos += 2;
            }
        } else {
            malformed("HMP");
        }
    }
}

[[nodiscard]] std::vector<std::uint8_t> convert_hmp(Bytes data)
{
    // HMIMIDIP is the classic layout; HMIMIDIR is the "funky" variant with a different header,
    // 16-bit track sizes and no post-track pad.
    const bool is_funky = u8(data, 7) == 'R';
    const std::size_t header_offset = is_funky ? 0x1A : 0x30;
    if (header_offset >= data.size()) {
        malformed("HMP");
    }

    const std::uint8_t track_count = u8(data, header_offset);
    if (track_count == 0) {
        malformed("HMP");
    }

    std::uint16_t division = 0xC0;
    if (is_funky) {
        if (data.size() <= 0x4D) {
            malformed("HMP");
        }
        // The reference reads this as a sparse 24-bit field with the middle byte implicitly
        // zero; the shape is preserved.
        division = static_cast<std::uint16_t>((static_cast<std::uint32_t>(u8(data, 0x4C)) << 16)
                                              | u8(data, 0x4D));
        if (division == 0) {
            malformed("HMP");
        }
    }

    std::vector<TickTrack> tracks;
    TickTrack conductor;
    push_meta(conductor, 0, meta_set_tempo, hmi_default_tempo);
    tracks.push_back(std::move(conductor));

    // Skip the first HMP "track" header by sliding a two-byte window to FF 2F, starting at the
    // track-count byte itself, matching the reference.
    std::size_t pos = header_offset;
    std::uint8_t previous = 0;
    bool found = false;
    while (pos < data.size()) {
        const std::uint8_t byte = data[pos];
        ++pos;
        if (previous == 0xFF && byte == 0x2F) {
            found = true;
            break;
        }
        previous = byte;
    }
    if (!found) {
        malformed("HMP");
    }

    const std::size_t post_header_skip = is_funky ? 3 : 5;
    if (data.size() - pos < post_header_skip) {
        malformed("HMP");
    }
    pos += post_header_skip;

    for (std::uint8_t i = 1; i < track_count; ++i) {
        std::size_t body_length = 0;
        std::size_t pre_skip = 0;

        if (is_funky) {
            if (data.size() - pos < 4) {
                break;
            }
            const std::uint16_t size16 = le16(data, pos);
            pos += 2;
            if (size16 < 4) {
                malformed("HMP");
            }
            body_length = static_cast<std::size_t>(size16) - 4;
            pre_skip = 2;
        } else {
            if (data.size() - pos < 8) {
                break;
            }
            const std::uint32_t size32 = le32(data, pos);
            pos += 4;
            if (size32 < 12) {
                malformed("HMP");
            }
            body_length = static_cast<std::size_t>(size32) - 12;
            pre_skip = 4;
        }

        if (data.size() - pos < body_length + pre_skip) {
            break;
        }
        pos += pre_skip;

        TickTrack track;
        parse_hmp_track(data.subspan(pos, body_length), track);
        tracks.push_back(std::move(track));

        pos += body_length + (is_funky ? 0 : 4);
    }

    return write_smf(1, division, std::move(tracks));
}

// ── HMI-MIDISONG ──────────────────────────────────────────────────────────────────────────────

[[nodiscard]] bool is_hmi(Bytes data)
{
    static constexpr char magic[] = "HMI-MIDISONG";
    if (data.size() < 12) {
        return false;
    }
    for (std::size_t i = 0; i < 12; ++i) {
        if (u8(data, i) != static_cast<std::uint8_t>(magic[i])) {
            return false;
        }
    }
    return true;
}

void parse_hmi_track(Bytes body, TickTrack& track, TickTrack& conductor)
{
    std::size_t pos = 0;
    std::int64_t tick = 0;
    std::int64_t last_event_tick = 0;
    int last_status = -1; // no prior voice status

    while (pos < body.size()) {
        const std::int64_t delta = read_vlq(body, pos);

        // The reference "shunt": an absurd delta (beyond 65535) is treated as a reset to the last
        // known event tick, guarding against a class of corrupt HMI files seen in the wild.
        if (delta > 0xFFFF) {
            tick = last_event_tick;
        } else {
            tick += delta;
            last_event_tick = std::max(last_event_tick, tick);
        }

        if (pos >= body.size()) {
            malformed("HMI");
        }
        const std::uint8_t status = body[pos];
        ++pos;

        if (status == 0xFF) {
            last_status = -1;
            if (pos >= body.size()) {
                malformed("HMI");
            }
            const std::uint8_t meta_type = body[pos];
            ++pos;
            const std::size_t meta_length = read_vlq(body, pos);
            if (body.size() - pos < meta_length) {
                malformed("HMI");
            }

            // End of Track slides forward to cover pending synthesised note-offs.
            if (meta_type == meta_end_of_track && tick < last_event_tick) {
                tick = last_event_tick;
            }

            push_meta(track, tick, meta_type, body.subspan(pos, meta_length));
            pos += meta_length;
            if (meta_type == meta_end_of_track) {
                break;
            }

        } else if (status == 0xF0) {
            last_status = -1;
            const std::size_t sysex_length = read_vlq(body, pos);
            if (body.size() - pos < sysex_length) {
                malformed("HMI");
            }
            push_sysex(track, tick, body.subspan(pos, sysex_length));
            pos += sysex_length;

        } else if (status == 0xFE) {
            // HMI-specific sub-event. 0x14 and 0x15 are the loop points, emitted as the marker
            // strings the loop scanner recognises; the rest are skipped by their fixed sizes.
            last_status = -1;
            if (pos >= body.size()) {
                malformed("HMI");
            }
            const std::uint8_t sub = body[pos];
            ++pos;
            switch (sub) {
            case 0x10: {
                if (body.size() - pos < 3) {
                    malformed("HMI");
                }
                pos += 2;
                const std::uint8_t extra = body[pos];
                ++pos;
                if (body.size() - pos < static_cast<std::size_t>(extra) + 4) {
                    malformed("HMI");
                }
                pos += static_cast<std::size_t>(extra) + 4;
                break;
            }
            case 0x12:
                if (body.size() - pos < 2) {
                    malformed("HMI");
                }
                pos += 2;
                break;
            case 0x13:
                if (body.size() - pos < 10) {
                    malformed("HMI");
                }
                pos += 10;
                break;
            case 0x14: {
                if (body.size() - pos < 2) {
                    malformed("HMI");
                }
                pos += 2;
                static constexpr char marker[] = "loopStart";
                push_meta(conductor,
                          tick,
                          meta_marker,
                          Bytes{reinterpret_cast<const std::uint8_t*>(marker), 9});
                break;
            }
            case 0x15: {
                if (body.size() - pos < 6) {
                    malformed("HMI");
                }
                pos += 6;
                static constexpr char marker[] = "loopEnd";
                push_meta(conductor,
                          tick,
                          meta_marker,
                          Bytes{reinterpret_cast<const std::uint8_t*>(marker), 7});
                break;
            }
            default:
                malformed("HMI");
            }

        } else if (status <= 0xEF) {
            // Voice event, possibly running status.
            std::uint8_t actual_status = 0;
            std::uint8_t d1 = 0;
            if (status >= 0x80) {
                actual_status = status;
                if (pos >= body.size()) {
                    malformed("HMI");
                }
                d1 = body[pos];
                ++pos;
                last_status = status;
            } else {
                // Running status: the byte already read is the first data byte.
                if (last_status < 0) {
                    malformed("HMI");
                }
                actual_status = static_cast<std::uint8_t>(last_status);
                d1 = status;
            }

            const std::uint8_t type = actual_status & 0xF0U;
            if (type == 0xC0 || type == 0xD0) {
                push_voice(track, tick, actual_status, d1);
            } else {
                if (pos >= body.size()) {
                    malformed("HMI");
                }
                push_voice(track, tick, actual_status, d1, body[pos]);
                ++pos;
            }

            // Note-on stores a duration; synthesise the matching note-off.
            if (type == 0x90) {
                const std::int64_t duration = read_vlq(body, pos);
                const std::int64_t note_end = tick + duration;
                last_event_tick = std::max(last_event_tick, note_end);
                push_voice(track, note_end, actual_status, d1, 0);
            }

        } else {
            malformed("HMI");
        }
    }
}

[[nodiscard]] std::vector<std::uint8_t> convert_hmi(Bytes data)
{
    if (data.size() < 0xEC) {
        malformed("HMI");
    }

    const std::uint32_t track_count = le32(data, 0xE4);
    const std::uint32_t table_offset = le32(data, 0xE8);
    if (track_count == 0 || table_offset >= data.size()
        || static_cast<std::uint64_t>(table_offset) + static_cast<std::uint64_t>(track_count) * 4
               > data.size()) {
        malformed("HMI");
    }

    std::vector<std::uint32_t> offsets(track_count);
    for (std::uint32_t i = 0; i < track_count; ++i) {
        offsets[i] = le32(data, table_offset + static_cast<std::size_t>(i) * 4);
    }

    std::vector<TickTrack> tracks;
    TickTrack conductor;
    push_meta(conductor, 0, meta_set_tempo, hmi_default_tempo);
    tracks.push_back(std::move(conductor));

    static constexpr char track_magic[] = "HMI-MIDITRACK";

    for (std::uint32_t i = 0; i < track_count; ++i) {
        const std::uint32_t offset = offsets[i];
        std::uint32_t length = 0;
        if (i + 1 < track_count) {
            if (offsets[i + 1] <= offset) {
                malformed("HMI");
            }
            length = offsets[i + 1] - offset;
        } else {
            if (offset >= data.size()) {
                malformed("HMI");
            }
            length = static_cast<std::uint32_t>(data.size() - offset);
        }
        if (static_cast<std::uint64_t>(offset) + length > data.size() || length < 13) {
            malformed("HMI");
        }
        for (std::size_t k = 0; k < 13; ++k) {
            if (u8(data, offset + k) != static_cast<std::uint8_t>(track_magic[k])) {
                malformed("HMI");
            }
        }

        TickTrack track;

        // Optional text metadata at a track-relative offset: a two-byte prefix, then the text,
        // trailing spaces trimmed.
        if (length < 0x4B + 4) {
            malformed("HMI");
        }
        const std::uint32_t meta_offset = le32(data, offset + 0x4B);
        if (meta_offset != 0 && static_cast<std::uint64_t>(meta_offset) + 1 < length) {
            const std::uint8_t meta_size = u8(data, offset + meta_offset + 1);
            if (static_cast<std::uint64_t>(meta_offset) + 2 + meta_size > length) {
                malformed("HMI");
            }
            std::size_t trimmed = meta_size;
            const std::size_t text_start = offset + meta_offset + 2;
            while (trimmed > 0 && u8(data, text_start + trimmed - 1) == ' ') {
                --trimmed;
            }
            if (trimmed > 0) {
                push_meta(track, 0, meta_text, data.subspan(text_start, trimmed));
            }
        }

        if (length < 0x57 + 4) {
            malformed("HMI");
        }
        const std::uint32_t data_offset = le32(data, offset + 0x57);
        if (data_offset > length) {
            malformed("HMI");
        }
        parse_hmi_track(data.subspan(offset + data_offset, length - data_offset),
                        track,
                        tracks[0]);
        tracks.push_back(std::move(track));
    }

    return write_smf(1, 0xC0, std::move(tracks));
}

// ── Mobile XMF ────────────────────────────────────────────────────────────────────────────────
//
// The header tree per RP-030 / RP-042 is walked for the embedded SMF FileNode. DLS FileNodes --
// the other thing the tree can carry -- are skipped: this engine renders from its own ROM.

[[nodiscard]] bool is_xmf(Bytes data)
{
    return data.size() >= 8 && tag_at(data, 0, "XMF_");
}

[[nodiscard]] std::vector<std::uint8_t>
xmf_inflate([[maybe_unused]] Bytes payload, [[maybe_unused]] std::size_t decoded_size)
{
#if defined(TS_HAVE_ZLIB)
    if (payload.empty() || decoded_size == 0) {
        malformed("XMF");
    }
    std::vector<std::uint8_t> out(decoded_size);
    uLongf dest_length = static_cast<uLongf>(decoded_size);
    const int rc = uncompress(out.data(), &dest_length, payload.data(),
                              static_cast<uLong>(payload.size()));
    if (rc != Z_OK) {
        malformed("XMF");
    }
    out.resize(dest_length);
    return out;
#else
    throw std::runtime_error(
        "This XMF file holds compressed content, and this build has no zlib to inflate it.");
#endif
}

/// Processes one node of the XMF tree. `node` spans the node's full byte length, starting at its
/// length quantity.
void xmf_process_node(Bytes node, std::optional<std::vector<std::uint8_t>>& midi_out)
{
    std::size_t pos = 0;

    const std::size_t node_length = read_vlq(node, pos);
    const std::size_t item_count = read_vlq(node, pos);
    const std::size_t header_size = read_vlq(node, pos);
    if (node_length > node.size() || header_size > node_length) {
        malformed("XMF");
    }

    // Metadata table: only the resource format matters here; titles and comments are RMIDI-info
    // fields this engine has no use for.
    const std::size_t metadata_size = read_vlq(node, pos);
    const std::size_t metadata_end = pos + metadata_size;
    if (metadata_end > header_size) {
        malformed("XMF");
    }

    bool has_resource_format = false;
    int format_type_id = -1;
    int resource_format_id = -1;

    while (pos < metadata_end) {
        // FieldSpecifier: either a numbered standard field or a named one.
        const std::uint8_t first = u8(node, pos);
        std::size_t field_id = static_cast<std::size_t>(-1);
        bool is_named = false;

        if (first == 0) {
            ++pos;
            field_id = read_vlq(node, pos);
        } else {
            const std::size_t name_length = read_vlq(node, pos);
            pos += name_length;
            is_named = true;
        }
        if (pos > metadata_end) {
            malformed("XMF");
        }

        const std::size_t version_count = read_vlq(node, pos);
        if (version_count == 0) {
            // UniversalContents only.
            const std::size_t data_length = read_vlq(node, pos);
            const std::size_t data_start = pos;
            if (data_start + data_length > metadata_end) {
                malformed("XMF");
            }

            (void)read_vlq(node, pos); // string-format id; unused without text fields
            const std::size_t consumed = pos - data_start;
            const std::size_t payload_length =
                data_length > consumed ? data_length - consumed : 0;

            // Field 3 is ResourceFormat: [FormatTypeID, StandardResourceFormatID].
            if (!is_named && payload_length > 0 && field_id == 3) {
                format_type_id = u8(node, pos);
                if (payload_length >= 2) {
                    resource_format_id = u8(node, pos + 1);
                }
                has_resource_format = true;
            }
            pos = data_start + data_length;
        } else {
            // International contents: skip whole.
            const std::size_t international_length = read_vlq(node, pos);
            pos += international_length;
        }
    }
    pos = metadata_end;

    // Unpackers: a non-empty block means the payload is compressed.
    bool packed = false;
    std::size_t decoded_size = 0;

    const std::size_t unpackers_start = pos;
    const std::size_t unpackers_length = read_vlq(node, pos);
    const std::size_t unpackers_end = unpackers_start + unpackers_length;
    if (unpackers_end > header_size) {
        malformed("XMF");
    }

    if (unpackers_length > 0) {
        packed = true;
        while (pos < unpackers_end) {
            const std::size_t unpacker_id = read_vlq(node, pos);
            if (unpacker_id == 0) {
                (void)read_vlq(node, pos); // StandardUnpackerID
            } else if (unpacker_id == 1) {
                const std::uint8_t manufacturer = u8(node, pos);
                ++pos;
                if (manufacturer == 0) {
                    pos += 2; // three-byte manufacturer id
                }
                (void)read_vlq(node, pos);
            } else {
                malformed("XMF"); // registered/non-registered unpackers are unsupported
            }
            decoded_size = read_vlq(node, pos);
        }
    }

    // Body: the reference type is at header_size. Only inline content is supported.
    pos = header_size;
    const std::size_t ref_type = read_vlq(node, pos);
    if (ref_type != 1) {
        malformed("XMF");
    }

    const std::size_t payload_start = pos;
    if (payload_start > node_length) {
        malformed("XMF");
    }
    const std::size_t payload_size = node_length - payload_start;

    if (item_count == 0) {
        // FileNode: interpret by resource format. Standard formats 0 and 1 are SMF; 2..5 are the
        // DLS family, skipped.
        if (has_resource_format && format_type_id == 0
            && (resource_format_id == 0 || resource_format_id == 1)) {
            const Bytes payload = node.subspan(payload_start, payload_size);
            if (packed) {
                midi_out = xmf_inflate(payload, decoded_size);
            } else {
                midi_out = std::vector<std::uint8_t>{payload.begin(), payload.end()};
            }
        }
    } else {
        // FolderNode: iterate the sub-nodes.
        const Bytes folder = node.subspan(payload_start, payload_size);
        std::size_t sub_pos = 0;
        std::size_t children_seen = 0;
        while (sub_pos < folder.size() && children_seen < item_count) {
            const std::size_t sub_start = sub_pos;
            const std::size_t sub_length = read_vlq(folder, sub_pos);
            if (sub_length == 0 || sub_start + sub_length > folder.size()) {
                malformed("XMF");
            }
            xmf_process_node(folder.subspan(sub_start, sub_length), midi_out);
            sub_pos = sub_start + sub_length;
            ++children_seen;
        }
    }
}

[[nodiscard]] std::vector<std::uint8_t> convert_xmf(Bytes data)
{
    std::size_t pos = 8;
    // Version 2.00 (RP-042) carries an additional FileTypeID and FileTypeRevisionID pair.
    if (tag_at(data, 4, "2.00")) {
        pos += 8;
    }

    (void)read_vlq(data, pos); // FileLength -- informational

    const std::size_t metadata_length = read_vlq(data, pos);
    if (pos + metadata_length > data.size()) {
        malformed("XMF");
    }
    pos += metadata_length;

    const std::size_t tree_start = read_vlq(data, pos);
    if (tree_start >= data.size()) {
        malformed("XMF");
    }

    std::size_t root_pos = tree_start;
    std::size_t root_length = read_vlq(data, root_pos);
    if (root_length == 0 || tree_start + root_length > data.size()) {
        root_length = data.size() - tree_start;
    }

    std::optional<std::vector<std::uint8_t>> midi;
    xmf_process_node(data.subspan(tree_start, root_length), midi);
    if (!midi) {
        throw std::runtime_error("This XMF file carries no Standard MIDI File node.");
    }
    return std::move(*midi);
}

// ── Loudness Sound System (LDS) ───────────────────────────────────────────────────────────────
//
// An AdLib FM tracker. Rather than mapping the file, the tracker's 70 Hz playback loop is
// simulated and each tick's state changes are translated into MIDI events, at TPQN 35 so one
// tracker tick is one MIDI tick under the default 500000 us/beat tempo. The reference port
// compiles its vibrato and tremolo paths out and its pitch-wheel glide path in; this port keeps
// exactly the enabled behaviour. Detection needs the file name: the format has no magic.

constexpr int lds_wheel_range = 12;

[[nodiscard]] std::uint8_t lds_wheel_low(int tune)
{
    return static_cast<std::uint8_t>(((tune * 512) / lds_wheel_range) & 127);
}

[[nodiscard]] std::uint8_t lds_wheel_high(int tune)
{
    return static_cast<std::uint8_t>(((((tune * 512) / lds_wheel_range) >> 7) + 64) & 127);
}

[[nodiscard]] bool is_lds(Bytes data, std::string_view name)
{
    if (name.size() < 4 || data.empty() || data[0] > 2) {
        return false;
    }
    const std::string_view ext = name.substr(name.size() - 4);
    return ext[0] == '.' && (ext[1] == 'l' || ext[1] == 'L') && (ext[2] == 'd' || ext[2] == 'D')
           && (ext[3] == 's' || ext[3] == 'S');
}

struct LdsPatch {
    std::uint8_t keyoff = 0;
    std::uint8_t portamento = 0;
    std::int8_t glide = 0;
    std::uint8_t midi_instrument = 0;
    std::uint8_t midi_velocity = 0;
    std::int8_t midi_transpose = 0;
};

struct LdsChannel {
    std::int16_t gototune = 0;
    std::int16_t lasttune = 0;
    std::uint16_t packpos = 0;
    std::int8_t finetune = 0;
    std::uint8_t glideto = 0;
    std::uint8_t portspeed = 0;
    std::uint8_t nextvol = 0;
    std::uint8_t keycount = 0;
    std::uint8_t packwait = 0;
    struct {
        std::uint8_t chandelay = 0;
        std::uint8_t sound = 0;
        std::uint16_t high = 0;
    } cheat;
};

struct LdsSimulation {
    std::vector<LdsPatch> patches;
    std::array<LdsChannel, 9> channel{};
    std::array<std::uint8_t, 9> current_instrument{};
    std::array<std::uint8_t, 9> last_channel{};
    std::array<std::uint8_t, 9> last_instrument{};
    std::array<std::uint8_t, 9> last_note{};
    std::array<std::uint8_t, 9> last_volume{};
    std::array<std::uint8_t, 11> last_sent_volume{};
    std::array<std::int16_t, 11> last_pitch_wheel{};

    std::array<TickTrack, 9> tracks{};
    TickTrack conductor;

    void emit_cc(std::size_t chan, std::int64_t tick, std::uint8_t midi_channel, std::uint8_t cc,
                 std::uint8_t value)
    {
        push_voice(tracks[chan], tick, static_cast<std::uint8_t>(0xB0U | (midi_channel & 0x0FU)),
                   cc, value);
    }

    void emit_wheel(std::size_t chan, std::int64_t tick, std::uint8_t midi_channel, int tune)
    {
        push_voice(tracks[chan], tick, static_cast<std::uint8_t>(0xE0U | (midi_channel & 0x0FU)),
                   lds_wheel_low(tune), lds_wheel_high(tune));
    }

    void play_sound(std::uint8_t allvolume,
                    std::int64_t now,
                    unsigned sound,
                    std::size_t chan,
                    unsigned high)
    {
        LdsChannel& c = channel[chan];
        current_instrument[chan] = static_cast<std::uint8_t>(sound);
        if (sound >= patches.size()) {
            return;
        }
        const LdsPatch& patch = patches[current_instrument[chan]];
        const unsigned midi_channel =
            patch.midi_instrument >= 0x80 ? 9U : (chan == 8 + 1 ? 10U : static_cast<unsigned>(chan));
        const unsigned saved_last_note = last_note[chan];
        unsigned note = 0;

        if (midi_channel != 9) {
            high = static_cast<unsigned>(static_cast<int>(high) + c.finetune);
            high = static_cast<unsigned>(static_cast<int>(high)
                                         + (static_cast<int>(patch.midi_transpose) << 4));
            note = static_cast<unsigned>(static_cast<int>(high) - c.lasttune);

            if (c.glideto != 0) {
                c.gototune = static_cast<std::int16_t>(
                    static_cast<int>(note) - (static_cast<int>(last_note[chan]) << 4)
                    + c.lasttune);
                c.portspeed = c.glideto;
                c.glideto = 0;
                c.finetune = 0;
                return;
            }

            if (patch.midi_instrument != last_instrument[chan]) {
                push_voice(tracks[chan],
                           now,
                           static_cast<std::uint8_t>(0xC0U | midi_channel),
                           patch.midi_instrument);
                last_instrument[chan] = patch.midi_instrument;
            }
        } else {
            note = static_cast<unsigned>((patch.midi_instrument & 0x7FU) << 4);
        }

        unsigned volume = 127;
        if (c.nextvol != 0) {
            volume = (c.nextvol & 0x3FU) * 127U / 63U;
            last_volume[chan] = static_cast<std::uint8_t>(volume);
        }
        if (allvolume != 0) {
            volume = volume * allvolume / 255U;
        }

        if (volume != last_sent_volume[last_channel[chan]]) {
            emit_cc(chan, now, last_channel[chan], 7, static_cast<std::uint8_t>(volume));
            last_sent_volume[last_channel[chan]] = static_cast<std::uint8_t>(volume);
        }

        if (saved_last_note != 0xFF) {
            push_voice(tracks[chan],
                       now,
                       static_cast<std::uint8_t>(0x80U | last_channel[chan]),
                       static_cast<std::uint8_t>(saved_last_note),
                       127);
            last_note[chan] = 0xFF;
            if (midi_channel != 9) {
                note = static_cast<unsigned>(static_cast<int>(note) + c.lasttune);
                c.lasttune = 0;
                if (last_pitch_wheel[midi_channel] != 0) {
                    emit_wheel(chan, now, last_channel[chan], 0);
                    last_pitch_wheel[midi_channel] = 0;
                }
            }
        }
        if (c.lasttune != last_pitch_wheel[midi_channel]) {
            emit_wheel(chan, now, static_cast<std::uint8_t>(midi_channel), c.lasttune);
            last_pitch_wheel[midi_channel] = c.lasttune;
        }

        if (patch.glide == 0 || last_note[chan] == 0xFF) {
            if (patch.portamento == 0 || last_note[chan] == 0xFF) {
                push_voice(tracks[chan],
                           now,
                           static_cast<std::uint8_t>(0x90U | midi_channel),
                           static_cast<std::uint8_t>(note >> 4),
                           patch.midi_velocity);
                last_note[chan] = static_cast<std::uint8_t>(note >> 4);
                last_channel[chan] = static_cast<std::uint8_t>(midi_channel);
                c.gototune = c.lasttune;
            } else {
                c.gototune = static_cast<std::int16_t>(
                    static_cast<int>(note) - (static_cast<int>(last_note[chan]) << 4)
                    + c.lasttune);
                c.portspeed = patch.portamento;
                last_note[chan] = static_cast<std::uint8_t>(saved_last_note);
                push_voice(tracks[chan],
                           now,
                           static_cast<std::uint8_t>(0x90U | midi_channel),
                           static_cast<std::uint8_t>(saved_last_note),
                           patch.midi_velocity);
            }
        } else {
            push_voice(tracks[chan],
                       now,
                       static_cast<std::uint8_t>(0x90U | midi_channel),
                       static_cast<std::uint8_t>(note >> 4),
                       patch.midi_velocity);
            last_note[chan] = static_cast<std::uint8_t>(note >> 4);
            last_channel[chan] = static_cast<std::uint8_t>(midi_channel);
            c.gototune = patch.glide;
            c.portspeed = patch.portamento;
        }

        c.glideto = 0;
        c.keycount = patch.keyoff;
        c.nextvol = 0;
        c.finetune = 0;
    }
};

[[nodiscard]] std::vector<std::uint8_t> convert_lds(Bytes data)
{
    std::size_t pos = 0;
    const std::uint8_t mode = u8(data, pos);
    ++pos;
    if (mode > 2 || data.size() - pos < 4) {
        malformed("LDS");
    }
    // The 16-bit speed field is unused by the reference.
    std::uint8_t tempo = u8(data, pos + 2);
    const std::uint8_t pattern_length = u8(data, pos + 3);
    pos += 4;

    if (data.size() - pos < 10) {
        malformed("LDS");
    }
    std::array<std::uint8_t, 9> channel_delay{};
    for (std::size_t i = 0; i < 9; ++i) {
        channel_delay[i] = u8(data, pos);
        ++pos;
    }
    ++pos; // register_bd -- unused

    if (data.size() - pos < 2) {
        malformed("LDS");
    }
    const std::uint16_t patch_count = le16(data, pos);
    pos += 2;
    if (patch_count == 0 || data.size() - pos < static_cast<std::size_t>(patch_count) * 46) {
        malformed("LDS");
    }

    LdsSimulation sim;
    sim.patches.resize(patch_count);
    for (LdsPatch& patch : sim.patches) {
        pos += 11;
        patch.keyoff = u8(data, pos);
        ++pos;
        patch.portamento = u8(data, pos);
        ++pos;
        patch.glide = static_cast<std::int8_t>(u8(data, pos));
        ++pos;
        pos += 1 + 2 + 3; // finetune pad, vibrato pair, tremolo trio -- disabled paths
        pos += 20;        // arpeggio and digital-instrument fields -- unused
        patch.midi_instrument = u8(data, pos);
        ++pos;
        patch.midi_velocity = u8(data, pos);
        ++pos;
        ++pos; // midi_key -- unused by the enabled paths
        patch.midi_transpose = static_cast<std::int8_t>(u8(data, pos));
        ++pos;
        pos += 2;

        // Drum patches do not glide (the reference's own workaround).
        if (patch.midi_instrument >= 0x80) {
            patch.glide = 0;
        }
    }

    if (data.size() - pos < 2) {
        malformed("LDS");
    }
    const std::uint16_t position_count = le16(data, pos);
    pos += 2;
    if (position_count == 0
        || data.size() - pos < static_cast<std::size_t>(position_count) * 27) {
        malformed("LDS");
    }

    struct Position {
        std::uint16_t pattern_number = 0;
        std::uint8_t transpose = 0;
    };
    std::vector<Position> positions(static_cast<std::size_t>(position_count) * 9);
    for (Position& position : positions) {
        const std::uint16_t packed = le16(data, pos);
        if ((packed & 1U) != 0) {
            malformed("LDS");
        }
        position.pattern_number = static_cast<std::uint16_t>(packed >> 1);
        position.transpose = u8(data, pos + 2);
        pos += 3;
    }

    if (data.size() - pos < 2) {
        malformed("LDS");
    }
    pos += 2;

    const std::size_t pattern_count = (data.size() - pos) / 2;
    std::vector<std::uint16_t> patterns(std::max<std::size_t>(pattern_count, 1));
    for (std::size_t i = 0; i < pattern_count; ++i) {
        patterns[i] = le16(data, pos);
        pos += 2;
    }

    std::vector<std::int64_t> position_timestamps(position_count, -1);

    sim.last_instrument.fill(0xFF);
    sim.last_note.fill(0xFF);
    sim.last_volume.fill(127);
    sim.last_sent_volume.fill(127);

    // Conductor: default tempo plus a controller and pitch-wheel init across channels 0..10.
    push_meta(sim.conductor, 0, meta_set_tempo, xmi_default_tempo);
    for (std::uint8_t ch = 0; ch < 11; ++ch) {
        const auto cc_status = static_cast<std::uint8_t>(0xB0U | ch);
        push_voice(sim.conductor, 0, cc_status, 120, 0); // all sound off
        push_voice(sim.conductor, 0, cc_status, 121, 0); // reset controllers
        push_voice(sim.conductor, 0, cc_status, 0x65, 0);
        push_voice(sim.conductor, 0, cc_status, 0x64, 0); // RPN 0: pitch-bend range
        push_voice(sim.conductor, 0, cc_status, 0x06, lds_wheel_range);
        push_voice(sim.conductor, 0, cc_status, 0x26, 0);
        push_voice(sim.conductor, 0, static_cast<std::uint8_t>(0xE0U | ch), 0, 64);
    }

    // ── The tracker playback loop ─────────────────────────────────────────────────────────────
    std::uint8_t tempo_now = 3;
    std::uint8_t fadeonoff = 0;
    std::uint8_t allvolume = 0;
    std::uint8_t hardfade = 0;
    std::uint8_t pattplay = 0;
    std::uint16_t posplay = 0;
    std::uint16_t jumppos = 0;
    std::uint32_t mainvolume = 0;

    constexpr std::uint16_t maxsound = 0x3F;
    constexpr std::uint16_t maxpos = 0xFF;

    // The reference trusts the file to terminate. A malformed jump table can cycle forever, so
    // this port caps the simulation at four hours of tracker time instead.
    constexpr std::int64_t tick_limit = 70LL * 60 * 60 * 4;

    std::int64_t now = 0;
    bool playing = true;

    while (playing) {
        if (now > tick_limit) {
            malformed("LDS");
        }

        if (fadeonoff != 0) {
            if (fadeonoff <= 128) {
                if (allvolume > fadeonoff || allvolume == 0) {
                    allvolume = static_cast<std::uint8_t>(allvolume - fadeonoff);
                } else {
                    allvolume = 1;
                    fadeonoff = 0;
                    if (hardfade != 0) {
                        playing = false;
                        hardfade = 0;
                        for (LdsChannel& c : sim.channel) {
                            c.keycount = 1;
                        }
                    }
                }
            } else if (static_cast<unsigned>((allvolume + (0x100 - fadeonoff)) & 0xFF)
                       <= mainvolume) {
                allvolume = static_cast<std::uint8_t>(allvolume + 0x100 - fadeonoff);
            } else {
                allvolume = static_cast<std::uint8_t>(mainvolume);
                fadeonoff = 0;
            }
        }

        // Channel-delay triggers.
        for (std::size_t chan = 0; chan < 9; ++chan) {
            LdsChannel& c = sim.channel[chan];
            if (c.cheat.chandelay != 0) {
                --c.cheat.chandelay;
                if (c.cheat.chandelay == 0) {
                    sim.play_sound(allvolume, now, c.cheat.sound, chan, c.cheat.high);
                }
            }
        }

        // A new tracker row when the tempo counter has drained.
        if (tempo_now == 0) {
            if (pattplay == 0 && position_timestamps[posplay] < 0) {
                position_timestamps[posplay] = now;
            }

            bool vbreak = false;
            for (std::size_t chan = 0; chan < 9; ++chan) {
                LdsChannel& c = sim.channel[chan];
                if (c.packwait != 0) {
                    --c.packwait;
                    continue;
                }
                const std::uint16_t pattern_number =
                    positions[static_cast<std::size_t>(posplay) * 9 + chan].pattern_number;
                const std::uint8_t transpose =
                    positions[static_cast<std::size_t>(posplay) * 9 + chan].transpose;

                if (static_cast<std::size_t>(pattern_number) + c.packpos >= pattern_count) {
                    malformed("LDS");
                }

                const std::uint16_t comword = patterns[pattern_number + c.packpos];
                const auto comhi = static_cast<std::uint8_t>(comword >> 8);
                const auto comlo = static_cast<std::uint8_t>(comword & 0xFFU);

                if (comword != 0) {
                    if (comhi == 0x80) {
                        c.packwait = comlo;
                    } else if (comhi >= 0x80) {
                        switch (comhi) {
                        case 0xFF: {
                            const unsigned volume = (comlo & 0x3FU) * 127U / 63U;
                            sim.last_volume[chan] = static_cast<std::uint8_t>(volume);
                            if (volume != sim.last_sent_volume[sim.last_channel[chan]]) {
                                sim.emit_cc(chan, now, sim.last_channel[chan], 7,
                                            static_cast<std::uint8_t>(volume));
                                sim.last_sent_volume[sim.last_channel[chan]] =
                                    static_cast<std::uint8_t>(volume);
                            }
                            break;
                        }
                        case 0xFE:
                            tempo = static_cast<std::uint8_t>(comword & 0x3FU);
                            break;
                        case 0xFD:
                            c.nextvol = comlo;
                            break;
                        case 0xFC:
                            playing = false;
                            break;
                        case 0xFB:
                            c.keycount = 1;
                            break;
                        case 0xFA:
                            vbreak = true;
                            jumppos = static_cast<std::uint16_t>((posplay + 1) & maxpos);
                            break;
                        case 0xF9:
                            vbreak = true;
                            jumppos = static_cast<std::uint16_t>(comlo & maxpos);
                            if (jumppos <= posplay) {
                                // A backward jump is the song's loop; mark it and stop.
                                if (jumppos < position_timestamps.size()
                                    && position_timestamps[jumppos] >= 0) {
                                    static constexpr char start_marker[] = "loopStart";
                                    static constexpr char end_marker[] = "loopEnd";
                                    push_meta(sim.conductor,
                                              position_timestamps[jumppos],
                                              meta_marker,
                                              Bytes{reinterpret_cast<const std::uint8_t*>(
                                                        start_marker),
                                                    9});
                                    push_meta(sim.conductor,
                                              now + tempo - 1,
                                              meta_marker,
                                              Bytes{reinterpret_cast<const std::uint8_t*>(
                                                        end_marker),
                                                    7});
                                }
                                playing = false;
                            }
                            break;
                        case 0xF8:
                            c.lasttune = 0;
                            break;
                        case 0xF7:
                            break; // vibrato -- disabled path
                        case 0xF6:
                            c.glideto = comlo;
                            break;
                        case 0xF5:
                            c.finetune = static_cast<std::int8_t>(comlo);
                            break;
                        case 0xF4:
                            if (hardfade == 0) {
                                allvolume = comlo;
                                mainvolume = comlo;
                                fadeonoff = 0;
                            }
                            break;
                        case 0xF3:
                            if (hardfade == 0) {
                                fadeonoff = comlo;
                            }
                            break;
                        case 0xF2:
                            break; // tremolo -- disabled path
                        case 0xF1: {
                            const auto pan = static_cast<std::uint8_t>((comlo & 0x3FU) * 127U / 63U);
                            sim.emit_cc(chan, now, sim.last_channel[chan], 10, pan);
                            break;
                        }
                        case 0xF0: {
                            push_voice(sim.tracks[chan],
                                       now,
                                       static_cast<std::uint8_t>(0xC0U
                                                                 | (sim.last_channel[chan]
                                                                    & 0x0FU)),
                                       static_cast<std::uint8_t>(comlo & 0x7FU));
                            break;
                        }
                        default:
                            if (comhi < 0xA0) {
                                c.glideto = static_cast<std::uint8_t>(comhi & 0x1FU);
                            }
                            break;
                        }
                    } else {
                        std::uint8_t sound = 0;
                        std::uint16_t high = 0;
                        auto transp = static_cast<std::int8_t>(transpose << 1);
                        transp = static_cast<std::int8_t>(transp >> 1);

                        if ((transpose & 128U) != 0) {
                            sound = static_cast<std::uint8_t>((comlo + transp) & maxsound);
                            high = static_cast<std::uint16_t>(comhi << 4);
                        } else {
                            sound = static_cast<std::uint8_t>(comlo & maxsound);
                            high = static_cast<std::uint16_t>((comhi + transp) << 4);
                        }

                        if (channel_delay[chan] == 0) {
                            sim.play_sound(allvolume, now, sound, chan, high);
                        } else {
                            c.cheat.chandelay = channel_delay[chan];
                            c.cheat.sound = sound;
                            c.cheat.high = high;
                        }
                    }
                }
                ++c.packpos;
            }

            tempo_now = tempo;
            ++pattplay;
            if (vbreak) {
                pattplay = 0;
                for (LdsChannel& c : sim.channel) {
                    c.packpos = 0;
                    c.packwait = 0;
                }
                posplay = jumppos;
                if (posplay >= position_count) {
                    malformed("LDS");
                }
            } else if (pattplay >= pattern_length) {
                pattplay = 0;
                for (LdsChannel& c : sim.channel) {
                    c.packpos = 0;
                    c.packwait = 0;
                }
                posplay = static_cast<std::uint16_t>((posplay + 1) & maxpos);
                if (posplay >= position_count) {
                    playing = false;
                }
            }
        } else {
            --tempo_now;
        }

        // Per-channel effects: key-off and the pitch-wheel glide.
        for (std::size_t chan = 0; chan < 9; ++chan) {
            LdsChannel& c = sim.channel[chan];

            if (c.keycount > 0) {
                if (c.keycount == 1 && sim.last_note[chan] != 0xFF) {
                    push_voice(sim.tracks[chan],
                               now,
                               static_cast<std::uint8_t>(0x80U | sim.last_channel[chan]),
                               sim.last_note[chan],
                               127);
                    sim.last_note[chan] = 0xFF;
                    if (sim.last_pitch_wheel[sim.last_channel[chan]] != 0) {
                        sim.emit_wheel(chan, now, sim.last_channel[chan], 0);
                        sim.last_pitch_wheel[sim.last_channel[chan]] = 0;
                        c.lasttune = 0;
                        c.gototune = 0;
                    }
                }
                --c.keycount;
            }

            if (c.lasttune != c.gototune) {
                if (c.lasttune > c.gototune) {
                    if (c.lasttune - c.gototune < c.portspeed) {
                        c.lasttune = c.gototune;
                    } else {
                        c.lasttune = static_cast<std::int16_t>(c.lasttune - c.portspeed);
                    }
                } else {
                    if (c.gototune - c.lasttune < c.portspeed) {
                        c.lasttune = c.gototune;
                    } else {
                        c.lasttune = static_cast<std::int16_t>(c.lasttune + c.portspeed);
                    }
                }
                if (c.lasttune != sim.last_pitch_wheel[sim.last_channel[chan]]) {
                    sim.emit_wheel(chan, now, sim.last_channel[chan], c.lasttune);
                    sim.last_pitch_wheel[sim.last_channel[chan]] = c.lasttune;
                }
            }
        }

        ++now;
    }
    --now;

    // Final note-offs and pitch-wheel centring.
    for (std::size_t chan = 0; chan < 9; ++chan) {
        if (!sim.tracks[chan].empty() && sim.last_note[chan] != 0xFF) {
            const std::int64_t off_tick = now + sim.channel[chan].keycount;
            push_voice(sim.tracks[chan],
                       off_tick,
                       static_cast<std::uint8_t>(0x80U | sim.last_channel[chan]),
                       sim.last_note[chan],
                       127);
            if (sim.last_pitch_wheel[sim.last_channel[chan]] != 0) {
                sim.emit_wheel(chan, off_tick, sim.last_channel[chan], 0);
            }
        }
    }

    // Conductor plus the channel tracks that produced anything.
    std::vector<TickTrack> tracks;
    tracks.push_back(std::move(sim.conductor));
    for (TickTrack& track : sim.tracks) {
        if (!track.empty()) {
            tracks.push_back(std::move(track));
        }
    }
    return write_smf(1, 35, std::move(tracks));
}

} // namespace

// ── Dispatch ──────────────────────────────────────────────────────────────────────────────────

std::optional<std::vector<std::uint8_t>> to_smf(Bytes data, std::string_view name)
{
    if (tag_at(data, 0, "RIFF")) {
        // Two RIFF variants, told apart by the inner four-character code.
        if (is_mids(data)) {
            return convert_mids(data);
        }
        return convert_rmidi(data);
    }
    if (is_mus(data)) {
        return convert_mus(data);
    }
    if (is_xmi(data)) {
        return convert_xmi(data);
    }
    if (is_gmf(data)) {
        return convert_gmf(data);
    }
    if (is_hmp(data)) {
        return convert_hmp(data);
    }
    if (is_hmi(data)) {
        return convert_hmi(data);
    }
    if (is_lds(data, name)) {
        return convert_lds(data);
    }
    if (is_xmf(data)) {
        return convert_xmf(data);
    }
    return std::nullopt;
}

} // namespace ts::formats
