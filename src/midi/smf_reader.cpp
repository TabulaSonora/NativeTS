#include "tabulasonora/smf_reader.hpp"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <map>

namespace ts::smf {
namespace {

/// One event as it comes off a track, before the tempo map turns ticks into time.
struct RawEvent {
    std::int64_t tick = 0;
    /// Preserves the order events were read in, so merging tracks is deterministic.
    int order = 0;
    enum class Kind { tempo, sysex, channel } kind = Kind::channel;
    int status = 0;
    int data1 = 0;
    int data2 = 0;
    int port = 0;
    std::vector<std::uint8_t> bytes;
};

[[nodiscard]] std::uint16_t read_u16be(std::span<const std::uint8_t> data, std::size_t at)
{
    return static_cast<std::uint16_t>((data[at] << 8) | data[at + 1]);
}

[[nodiscard]] std::uint32_t read_u32be(std::span<const std::uint8_t> data, std::size_t at)
{
    return (static_cast<std::uint32_t>(data[at]) << 24)
           | (static_cast<std::uint32_t>(data[at + 1]) << 16)
           | (static_cast<std::uint32_t>(data[at + 2]) << 8)
           | static_cast<std::uint32_t>(data[at + 3]);
}

[[nodiscard]] bool starts_with(std::span<const std::uint8_t> data, std::size_t at, const char* tag)
{
    if (at + 4 > data.size()) {
        return false;
    }
    return std::memcmp(data.data() + at, tag, 4) == 0;
}

/// Reads a variable-length quantity, advancing the position.
std::size_t
read_variable_length(std::span<const std::uint8_t> data, std::size_t position, int& value)
{
    value = 0;
    while (true) {
        if (position >= data.size()) {
            throw std::runtime_error("Truncated variable-length quantity.");
        }
        const std::uint8_t byte = data[position];
        ++position;
        value = (value << 7) | (byte & 0x7F);
        if ((byte & 0x80) == 0) {
            return position;
        }
    }
}

} // namespace

std::int64_t quantise(double samples) noexcept
{
    // std::nearbyint under the default FE_TONEAREST is round-half-to-even, which is what .NET's
    // Math.Round does. Measured, the choice does not actually reach the block here -- see the
    // header -- but matching the original exactly costs nothing and does not depend on that
    // argument continuing to hold.
    return static_cast<std::int64_t>(std::nearbyint(samples)) / block_grid * block_grid;
}

std::vector<MidiEvent> read(const std::filesystem::path& path, int sample_rate)
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot read '" + path.string() + "'.");
    }
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>{stream},
                                         std::istreambuf_iterator<char>{}};
    return parse(data, sample_rate);
}

std::vector<MidiEvent> parse(std::span<const std::uint8_t> data, int sample_rate)
{
    if (data.size() < 14 || !starts_with(data, 0, "MThd")) {
        throw std::runtime_error("Not a Standard MIDI File: missing MThd.");
    }

    const std::uint16_t track_count = read_u16be(data, 10);
    const std::uint16_t division = read_u16be(data, 12);
    if ((division & 0x8000) != 0) {
        throw std::runtime_error("SMPTE division is not supported.");
    }

    const int ticks_per_quarter = division;
    std::size_t position = 14;
    std::vector<RawEvent> merged;

    // Device names to port numbers, assigned in order of first appearance and shared across
    // tracks: two tracks naming the same output belong to the same port, which is the whole point
    // of naming it.
    std::map<std::string, int> device_ports;
    int order = 0;

    for (int track = 0; track < track_count; ++track) {
        if (position + 8 > data.size() || !starts_with(data, position, "MTrk")) {
            throw std::runtime_error("Track " + std::to_string(track)
                                     + " is missing its MTrk header.");
        }

        const auto length = static_cast<std::size_t>(read_u32be(data, position + 4));
        position += 8;

        const std::size_t end = std::min(position + length, data.size());
        std::int64_t tick = 0;
        int status = 0;

        // The port a track is tagged for, and it is a *prefix*: it applies to the events that
        // follow it in the track, so a track that switches ports partway is honoured rather than
        // being forced to one. Tracks start on port 0, which is every untagged file.
        int track_port = 0;

        while (position < end) {
            int delta = 0;
            position = read_variable_length(data, position, delta);
            tick += delta;

            if (position >= end) {
                break;
            }

            int message = data[position];
            if ((message & 0x80) != 0) {
                ++position;
                // Only a channel message becomes the running status. Storing a meta or SysEx
                // status here is a desync waiting to happen: the next event that omits its status
                // byte would be read as another meta, its length byte swallowed as data, and the
                // rest of the track parsed at the wrong offsets. Africa.mid is what found it --
                // every track interleaves meta events with running-status notes, so each ran
                // correctly until its first meta and then lost about a third of itself, ending the
                // song at 90 s instead of 281.
                //
                // A system message does not *clear* the running status either. The spec says it
                // should, but sequencers write files that resume running status across a meta
                // event and players accept them; this file does exactly that, and clearing costs
                // it half its notes.
                if (message < 0xF0) {
                    status = message;
                }
            } else {
                // Running status: reuse the previous status byte.
                message = status;
                if (message == 0) {
                    // Running status with none set. The track is malformed from here, and guessing
                    // would produce plausible-sounding nonsense; stop reading it instead.
                    break;
                }
            }

            if (message == 0xFF) {
                const int meta_type = data[position];
                ++position;
                int meta_length = 0;
                position = read_variable_length(data, position, meta_length);

                if (meta_type == 0x21 && meta_length >= 1) {
                    // FF 21: MIDI Port. The number is taken as given -- clamping to the engine's
                    // width belongs to the engine, which masks, and doing it here would lose the
                    // file's intent for a wider one.
                    track_port = data[position];
                } else if ((meta_type == 0x09 || meta_type == 0x04) && meta_length >= 1) {
                    // FF 09: Device Name, and FF 04: Instrument Name serving as one -- files that
                    // predate FF 09 name their output there instead ("Modem" and "Printer", the
                    // Mac serial ports, in the ones that prompted this). There is no number in
                    // either, so names are assigned ports in order of first appearance, which is
                    // deterministic and matches what a sequencer means by listing outputs. Both
                    // metas share one table, keyed by the string exactly as stored. An explicit
                    // FF 21 later in the track still overrides it.
                    const std::string name(
                        reinterpret_cast<const char*>(data.data() + position),
                        static_cast<std::size_t>(meta_length));
                    const auto found = device_ports.find(name);
                    if (found != device_ports.end()) {
                        track_port = found->second;
                    } else {
                        const auto assigned = static_cast<int>(device_ports.size());
                        device_ports.emplace(name, assigned);
                        track_port = assigned;
                    }
                } else if (meta_type == 0x51 && meta_length >= 3) {
                    const int tempo =
                        (data[position] << 16) | (data[position + 1] << 8) | data[position + 2];
                    merged.push_back(
                        RawEvent{tick, order++, RawEvent::Kind::tempo, tempo, 0, 0, 0, {}});
                }

                position += static_cast<std::size_t>(meta_length);
            } else if (message == 0xF0 || message == 0xF7) {
                int sysex_length = 0;
                position = read_variable_length(data, position, sysex_length);

                if (message == 0xF0) {
                    // The stored payload omits the leading F0; put it back.
                    std::vector<std::uint8_t> bytes;
                    bytes.reserve(static_cast<std::size_t>(sysex_length) + 1);
                    bytes.push_back(0xF0);
                    bytes.insert(bytes.end(),
                                 data.begin() + static_cast<std::ptrdiff_t>(position),
                                 data.begin() + static_cast<std::ptrdiff_t>(position)
                                     + sysex_length);
                    merged.push_back(
                        RawEvent{tick, order++, RawEvent::Kind::sysex, 0, 0, 0, track_port, std::move(bytes)});
                }

                position += static_cast<std::size_t>(sysex_length);
            } else {
                const int type = message & 0xF0;
                if (type == 0xC0 || type == 0xD0) {
                    merged.push_back(RawEvent{tick,
                                              order++,
                                              RawEvent::Kind::channel,
                                              message,
                                              data[position],
                                              0,
                                              track_port,
                                              {}});
                    position += 1;
                } else {
                    merged.push_back(RawEvent{tick,
                                              order++,
                                              RawEvent::Kind::channel,
                                              message,
                                              data[position],
                                              data[position + 1],
                                              track_port,
                                              {}});
                    position += 2;
                }
            }
        }

        position = end;
    }

    // Merge the tracks by tick, then walk the tempo map to assign absolute time. The order field
    // makes this a total order, so no stability is required here.
    std::sort(merged.begin(), merged.end(), [](const RawEvent& a, const RawEvent& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });

    std::vector<MidiEvent> events;
    events.reserve(merged.size());

    int tempo_now = default_tempo;
    std::int64_t last_tick = 0;
    double seconds = 0.0;

    for (RawEvent& entry : merged) {
        seconds +=
            static_cast<double>(entry.tick - last_tick) * (tempo_now / 1e6 / ticks_per_quarter);
        last_tick = entry.tick;

        switch (entry.kind) {
        case RawEvent::Kind::tempo:
            tempo_now = entry.status;
            break;

        case RawEvent::Kind::sysex:
            events.push_back(MidiEvent{quantise(seconds * sample_rate),
                                       MidiEventKind::sysex,
                                       0,
                                       0,
                                       0,
                                       std::move(entry.bytes),
                                       entry.port});
            break;

        case RawEvent::Kind::channel:
            events.push_back(MidiEvent{quantise(seconds * sample_rate),
                                       MidiEventKind::channel,
                                       entry.status,
                                       entry.data1,
                                       entry.data2,
                                       {},
                                       entry.port});
            break;
        }
    }

    // A *stable* sort: same-position events must keep their original tick order. std::sort would be
    // free to reorder them, which reorders a program change against the note that follows it.
    std::stable_sort(events.begin(), events.end(), [](const MidiEvent& a, const MidiEvent& b) {
        return a.position < b.position;
    });

    return events;
}

} // namespace ts::smf
