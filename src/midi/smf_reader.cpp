#include "tabulasonora/smf_reader.hpp"

#include "tabulasonora/midi_formats.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
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
    /// The port each tagging scheme would give this event. Which one actually applies is not
    /// known until the whole file has been read -- see the selection after the parse loop -- so
    /// all three are carried.
    int port_by_number = 0;
    int port_by_device = 0;
    int port_by_instrument = 0;
    std::vector<std::uint8_t> bytes;
};

/// A loop marker seen during the parse, in ticks; resolved to samples once the tempo map exists.
///
/// Collected per track and only committed when the track survives EMIDI filtering: the reference
/// port rescans loops after dropping non-GM tracks, so a loop declared only in a dropped track
/// must not survive it here either.
struct LoopSignal {
    enum class Kind {
        touhou_start,
        touhou_end,
        /// A CC 2 or CC 4 with a non-zero value, which voids the whole Touhou scan.
        touhou_error,
        rpg_start,
        xmi_start,
        xmi_end,
        marker_start,
        marker_end,
    } kind = Kind::marker_start;
    std::int64_t tick = 0;
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

/// Lower-cases a marker payload and trims surrounding whitespace, for comparing loop markers.
[[nodiscard]] std::string lower_trim(std::span<const std::uint8_t> text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<int>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<int>(text[end - 1])) != 0) {
        --end;
    }
    std::string lowered;
    lowered.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<int>(text[i]))));
    }
    return lowered;
}

} // namespace

std::int64_t quantise(double samples) noexcept
{
    // The engine recalculates event timing onto **1 ms increments**, which at its 32 kHz internal
    // rate is exactly the 32 samples of `block_grid`. That is what this models -- not a render
    // block, which is what the constant used to claim and which is 320.
    //
    // The distinction matters because the wrong name invites the wrong fix. Coarsening this to 320
    // to "match the control block" was tried and measured: it collapses a note-on and its note-off
    // onto one boundary and takes shangai's channel 1 to 0.40 peak and 0.47 rms against the module.
    // Removing the grid entirely was also tried, and renders bit-identically on that file -- the
    // positions are already quantised downstream -- so the grid is not what is costing us anything.
    //
    // `std::nearbyint` under FE_TONEAREST is round-half-to-even, matching .NET's `Math.Round`, so
    // this and `scdec` land on the same integer for the same input. The engine's own rounding onto
    // the 1 ms grid is NOT yet established; half-to-even here is the assumption, not a measurement,
    // and is the first thing to check if event placement is ever suspected again.
    return static_cast<std::int64_t>(std::nearbyint(samples)) / block_grid * block_grid;
}

Song load(const std::filesystem::path& path, int sample_rate)
{
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot read '" + path.string() + "'.");
    }
    const std::vector<std::uint8_t> data{std::istreambuf_iterator<char>{stream},
                                         std::istreambuf_iterator<char>{}};
    return load(data, sample_rate, path.filename().string());
}

std::vector<MidiEvent> read(const std::filesystem::path& path, int sample_rate)
{
    return load(path, sample_rate).events;
}

// TODO: this parser is more permissive than the module's, and that permissiveness hides bugs
// rather than tolerating them.
//
// Found the way these things are always found. A generated probe in the 2026-08-08 key-shift work
// left a dangling delta-time before end-of-track -- a delta with no event after it, so the next
// byte read as a status was 0x00. `scdec` rejected the file outright ("Index was outside the
// bounds of the array") and this parser rendered it anyway, which meant the two engines were
// briefly being compared on inputs only one of them considered a MIDI file. The generator bug was
// mine and took a round to spot precisely because this side stayed quiet.
//
// Being lenient is defensible for a player asked to open whatever a user has. It is not defensible
// for the half of an oracle harness whose entire job is to be comparable to the other half: if the
// module will not parse something, rendering it here produces a number that means nothing, and
// silence is the worst possible way to report that. At minimum a trailing delta with no event, a
// track whose events overrun its declared length, and a status byte of 0x00 with no running status
// in force should be diagnosed rather than absorbed. Whether that is a hard error or a warning is
// a real question -- the corpus has real files with real damage in them, and `Africa.mid` below is
// one -- so the likely answer is strict by default with an opt-out, not strict everywhere.
std::vector<MidiEvent> parse(std::span<const std::uint8_t> data, int sample_rate)
{
    return load(data, sample_rate).events;
}

Song load(std::span<const std::uint8_t> data, int sample_rate, std::string_view name)
{
    // A foreign format -- MUS, XMI, an RMID or MIDS wrapper, and the rest -- is converted to an
    // in-memory Standard MIDI File first, so everything below stays one reader.
    std::vector<std::uint8_t> converted;
    if (auto foreign = formats::to_smf(data, name)) {
        converted = std::move(*foreign);
        data = converted;
    }

    if (data.size() < 14 || !starts_with(data, 0, "MThd")) {
        throw std::runtime_error("Not a Standard MIDI File: missing MThd.");
    }

    const std::uint16_t format = read_u16be(data, 8);
    const std::uint16_t track_count = read_u16be(data, 10);
    const std::uint16_t division = read_u16be(data, 12);
    if ((division & 0x8000) != 0) {
        throw std::runtime_error("SMPTE division is not supported.");
    }

    const int ticks_per_quarter = division;
    std::size_t position = 14;
    std::vector<RawEvent> merged;

    // Name-to-port tables, assigned in order of first appearance and shared across tracks: two
    // tracks naming the same output belong to the same port, which is the whole point of naming
    // it. FF 09 (Device Name) and FF 04 (Instrument Name) each get their own table because only
    // one scheme ends up applying -- see the selection after the parse loop -- and a file using
    // both must not have its FF 09 numbering shifted by FF 04 strings that will be ignored.
    std::map<std::string, int> device_ports;
    std::map<std::string, int> instrument_ports;

    // Which tagging schemes the file uses at all, decided over the whole file: FF 21 (MIDI Port)
    // outranks FF 09, which outranks FF 04.
    bool saw_port_number = false;
    bool saw_device_name = false;

    // Whether any MIDI channel is claimed under more than one distinct name -- per scheme. This
    // is the gate on the name schemes: names only mean ports when the same channel number is
    // claimed by two different names, which is the one thing sixteen channels cannot express by
    // themselves. Without a collision the names are what they say they are -- instrument labels
    // ("Flute", "CHA 1") in the FF 04 case -- and a file full of distinct labels must not be
    // scattered across ports it never asked for. Measured over a 128,000-file archive, 2,158
    // files carry two or more distinct FF 04 strings with no FF 21 or FF 09 in them, and nearly
    // all are single-port files with per-track labels.
    //
    // Only tracks that play exactly *one* channel get a vote. A named track spanning several
    // channels is a mix-down or an alternate take riding along in the file -- innerlight.mid
    // carries three "CHA n" tracks on channels 0-2 and one "original" track playing all sixteen,
    // and that shape must not read as a device collision -- whereas a track that is one device
    // voice, the only thing worth naming an output for, plays one channel.
    std::array<int, 16> device_name_on_channel;
    std::array<int, 16> instrument_name_on_channel;
    device_name_on_channel.fill(-1);
    instrument_name_on_channel.fill(-1);
    bool device_names_collide = false;
    bool instrument_names_collide = false;

    // Feeds one single-channel track's names into the gate above.
    const auto claim_channel = [](std::array<int, 16>& claims,
                                  bool& collide,
                                  int channel,
                                  std::span<const int> names) {
        for (const int name_id : names) {
            int& slot = claims[static_cast<std::size_t>(channel)];
            if (slot < 0) {
                slot = name_id;
            } else if (slot != name_id) {
                collide = true;
            }
        }
    };

    // What the loop scanners and the EMIDI filter need from the whole file: the committed loop
    // signals, whether any track carries an EMIDI designation, and the tick of the last voice
    // event (or final End of Track) across the tracks that survive.
    std::vector<LoopSignal> loop_signals;
    bool any_emidi = false;
    std::int64_t last_voice_event_tick = 0;

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

        // The port a track is tagged for under each scheme, and each is a *prefix*: it applies to
        // the events that follow it in the track, so a track that switches ports partway is
        // honoured rather than being forced to one. Tracks start on port 0, which is every
        // untagged file. The name ids run beside the ports (they carry the same number, since a
        // name's table id is its port) because the collision gate must tell "no name yet", which
        // is -1 here, from "the first name", which is port 0 either way.
        int track_port_by_number = 0;
        int track_port_by_device = 0;
        int track_port_by_instrument = 0;
        int track_device_name = -1;
        int track_instrument_name = -1;

        // What this track contributes to the collision gate, held back until the track ends,
        // because whether it votes at all depends on how many channels it turns out to play.
        std::uint16_t channels_used = 0;
        std::vector<int> device_names_used;
        std::vector<int> instrument_names_used;

        // The track's own loop signals and EMIDI verdict, also held back until the track ends: a
        // track designated for a non-GM synthesizer is dropped whole, and nothing it declared --
        // events or loop points -- may leak into the result. CC 110 is the EMIDI Track
        // Designation: values 0, 1 and 127 mean the track plays on a General MIDI receiver, any
        // other value targets a specific device (MT-32, SCC-1, ...). A song authored for several
        // synthesizers duplicates its content across tracks, so playing all of them as GM doubles
        // every voice.
        std::vector<LoopSignal> track_signals;
        bool track_emidi_any = false;
        bool track_emidi_other = false;
        std::int64_t track_last_voice_tick = 0;
        const std::size_t merged_before = merged.size();

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
                    track_port_by_number = data[position];
                    saw_port_number = true;
                } else if ((meta_type == 0x09 || meta_type == 0x04) && meta_length >= 1) {
                    // FF 09: Device Name, or FF 04: Instrument Name serving as one -- files that
                    // predate FF 09 name their output there instead ("Modem" and "Printer", the
                    // Mac serial ports, in the ones that prompted this). There is no number in
                    // either, so names are assigned ports in order of first appearance,
                    // deduplicated by the string exactly as stored, which is deterministic and
                    // matches what a sequencer means by listing outputs.
                    const std::string name_text(
                        reinterpret_cast<const char*>(data.data() + position),
                        static_cast<std::size_t>(meta_length));
                    auto& table = meta_type == 0x09 ? device_ports : instrument_ports;
                    auto& track_port = meta_type == 0x09 ? track_port_by_device
                                                         : track_port_by_instrument;
                    auto& track_name =
                        meta_type == 0x09 ? track_device_name : track_instrument_name;
                    const auto found = table.find(name_text);
                    if (found != table.end()) {
                        track_port = found->second;
                    } else {
                        const auto assigned = static_cast<int>(table.size());
                        table.emplace(name_text, assigned);
                        track_port = assigned;
                    }
                    track_name = track_port;
                    if (meta_type == 0x09) {
                        saw_device_name = true;
                    }
                } else if (meta_type == 0x51 && meta_length >= 3) {
                    const int tempo =
                        (data[position] << 16) | (data[position + 1] << 8) | data[position + 2];
                    merged.push_back(
                        RawEvent{tick, order++, RawEvent::Kind::tempo, tempo, 0, 0, 0, 0, 0, {}});
                } else if (meta_type == 0x06 && meta_length >= 1) {
                    // FF 06: Marker. Only the loop markers matter, and "start" is accepted as a
                    // loop-start alias.
                    const std::string marker = lower_trim(
                        data.subspan(position, static_cast<std::size_t>(meta_length)));
                    if (marker == "loopstart" || marker == "start") {
                        track_signals.push_back(LoopSignal{LoopSignal::Kind::marker_start, tick});
                    } else if (marker == "loopend") {
                        track_signals.push_back(LoopSignal{LoopSignal::Kind::marker_end, tick});
                    }
                } else if (meta_type == 0x2F) {
                    // FF 2F: End of Track. Its tick is the end of the longest track, which is
                    // what a start-only loop runs to.
                    track_last_voice_tick = std::max(track_last_voice_tick, tick);
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
                    merged.push_back(RawEvent{tick,
                                              order++,
                                              RawEvent::Kind::sysex,
                                              0,
                                              0,
                                              0,
                                              track_port_by_number,
                                              track_port_by_device,
                                              track_port_by_instrument,
                                              std::move(bytes)});
                }

                position += static_cast<std::size_t>(sysex_length);
            } else {
                // The collision gate's bookkeeping: the channels this track plays, and the names
                // in force while it plays them. Judged at the end of the track.
                channels_used |= static_cast<std::uint16_t>(1U << (message & 0x0F));
                if (track_device_name >= 0
                    && std::find(device_names_used.begin(),
                                 device_names_used.end(),
                                 track_device_name)
                           == device_names_used.end()) {
                    device_names_used.push_back(track_device_name);
                }
                if (track_instrument_name >= 0
                    && std::find(instrument_names_used.begin(),
                                 instrument_names_used.end(),
                                 track_instrument_name)
                           == instrument_names_used.end()) {
                    instrument_names_used.push_back(track_instrument_name);
                }

                track_last_voice_tick = std::max(track_last_voice_tick, tick);

                const int type = message & 0xF0;
                if (type == 0xC0 || type == 0xD0) {
                    merged.push_back(RawEvent{tick,
                                              order++,
                                              RawEvent::Kind::channel,
                                              message,
                                              data[position],
                                              0,
                                              track_port_by_number,
                                              track_port_by_device,
                                              track_port_by_instrument,
                                              {}});
                    position += 1;
                } else {
                    if (type == 0xB0) {
                        // The controllers the loop scanners and the EMIDI filter live on.
                        const int controller = data[position];
                        const int value = data[position + 1];
                        switch (controller) {
                        case 2: // Touhou loop start; a non-zero value voids the whole scan
                            track_signals.push_back(LoopSignal{
                                value == 0 ? LoopSignal::Kind::touhou_start
                                           : LoopSignal::Kind::touhou_error,
                                tick});
                            break;
                        case 4: // Touhou loop end
                            track_signals.push_back(LoopSignal{
                                value == 0 ? LoopSignal::Kind::touhou_end
                                           : LoopSignal::Kind::touhou_error,
                                tick});
                            break;
                        case 110: // EMIDI Track Designation
                            track_emidi_any = true;
                            if (value != 0 && value != 1 && value != 127) {
                                track_emidi_other = true;
                            }
                            break;
                        case 111: // RPG Maker loop start
                            if (value == 0) {
                                track_signals.push_back(
                                    LoopSignal{LoopSignal::Kind::rpg_start, tick});
                            }
                            break;
                        case 116: // XMI / EMIDI loop starts and ends
                        case 118:
                            track_signals.push_back(
                                LoopSignal{LoopSignal::Kind::xmi_start, tick});
                            break;
                        case 117:
                        case 119:
                            track_signals.push_back(LoopSignal{LoopSignal::Kind::xmi_end, tick});
                            break;
                        default:
                            break;
                        }
                    }

                    merged.push_back(RawEvent{tick,
                                              order++,
                                              RawEvent::Kind::channel,
                                              message,
                                              data[position],
                                              data[position + 1],
                                              track_port_by_number,
                                              track_port_by_device,
                                              track_port_by_instrument,
                                              {}});
                    position += 2;
                }
            }
        }

        if (track_emidi_other) {
            // The whole track was authored for some other synthesizer: drop everything it pushed,
            // and none of its votes or loop points count.
            merged.resize(merged_before);
        } else {
            any_emidi = any_emidi || track_emidi_any;
            last_voice_event_tick = std::max(last_voice_event_tick, track_last_voice_tick);
            loop_signals.insert(loop_signals.end(), track_signals.begin(), track_signals.end());

            // The track's vote: only a single-channel track can claim its channel for a name. Two
            // names from one such track still collide -- a prefix switch mid-track is a device
            // switch on that channel, and it needs the ports just as much as two tracks do.
            if (std::has_single_bit(channels_used)) {
                const int channel = std::countr_zero(channels_used);
                claim_channel(device_name_on_channel, device_names_collide, channel,
                              device_names_used);
                claim_channel(instrument_name_on_channel, instrument_names_collide, channel,
                              instrument_names_used);
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

    // One tagging scheme applies to the whole file, and it is only decidable now that all of it
    // has been read: FF 21 carries actual numbers, so its presence anywhere wins; FF 09 is the
    // meta defined for naming outputs, so it outranks FF 04, which only ever means a device in
    // files older than FF 09. A name scheme additionally has to pass the collision gate -- some
    // channel claimed under two names -- or its names are labels and it does not apply; a gated-
    // out FF 09 still yields to FF 04, for the file whose real devices are named there. A file
    // using none of them is all port 0, as it always was.
    const bool by_device = saw_device_name && device_names_collide;
    const bool by_instrument = instrument_names_collide;
    const auto port_of = [&](const RawEvent& entry) {
        if (saw_port_number) {
            return entry.port_by_number;
        }
        if (by_device) {
            return entry.port_by_device;
        }
        return by_instrument ? entry.port_by_instrument : 0;
    };

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
                                       port_of(entry)});
            break;

        case RawEvent::Kind::channel:
            events.push_back(MidiEvent{quantise(seconds * sample_rate),
                                       MidiEventKind::channel,
                                       entry.status,
                                       entry.data1,
                                       entry.data2,
                                       {},
                                       port_of(entry)});
            break;
        }
    }

    // A *stable* sort: same-position events must keep their original tick order. std::sort would be
    // free to reorder them, which reorders a program change against the note that follows it.
    std::stable_sort(events.begin(), events.end(), [](const MidiEvent& a, const MidiEvent& b) {
        return a.position < b.position;
    });

    // ── The loop scanners ─────────────────────────────────────────────────────────────────────
    //
    // Four independent dialects; the outermost surviving start and end win. Ported from
    // spessasynth_core_c's scan_loops, which ports midi_processing's scan_for_loops.
    constexpr std::int64_t unset = -1;
    std::int64_t loop_start = unset;
    std::int64_t loop_end = unset;
    bool soft = false;

    // Scan 1 -- Touhou (format 0 only): CC 2 is the start and CC 4 the end, both with value
    // zero; a non-zero value on either voids the entire result.
    if (format == 0) {
        bool errored = false;
        std::int64_t touhou_start = unset;
        std::int64_t touhou_end = unset;
        for (const LoopSignal& signal : loop_signals) {
            if (signal.kind == LoopSignal::Kind::touhou_error) {
                errored = true;
                break;
            }
            if (signal.kind == LoopSignal::Kind::touhou_start
                && (touhou_start == unset || signal.tick < touhou_start)) {
                touhou_start = signal.tick;
            } else if (signal.kind == LoopSignal::Kind::touhou_end) {
                if (touhou_end == unset || signal.tick > touhou_end) {
                    touhou_end = signal.tick;
                }
            }
        }
        if (!errored) {
            loop_start = touhou_start;
            loop_end = touhou_end;
            soft = touhou_end != unset;
        }
    }

    // Scan 2 -- RPG Maker: CC 111 is a loop start. Disabled when EMIDI is present anywhere,
    // because EMIDI gives CC 111 another meaning.
    if (!any_emidi) {
        for (const LoopSignal& signal : loop_signals) {
            if (signal.kind == LoopSignal::Kind::rpg_start
                && (loop_start == unset || signal.tick < loop_start)) {
                loop_start = signal.tick;
            }
        }
    }

    // Scan 3 -- XMI / EMIDI: CC 116/118 start, CC 117/119 end.
    for (const LoopSignal& signal : loop_signals) {
        if (signal.kind == LoopSignal::Kind::xmi_start
            && (loop_start == unset || signal.tick < loop_start)) {
            loop_start = signal.tick;
        } else if (signal.kind == LoopSignal::Kind::xmi_end) {
            if (loop_end == unset || signal.tick > loop_end) {
                loop_end = signal.tick;
            }
            soft = true;
        }
    }

    // Scan 4 -- Marker meta events: "loopStart" / "loopEnd", with "start" as a start alias.
    for (const LoopSignal& signal : loop_signals) {
        if (signal.kind == LoopSignal::Kind::marker_start
            && (loop_start == unset || signal.tick < loop_start)) {
            loop_start = signal.tick;
        } else if (signal.kind == LoopSignal::Kind::marker_end
                   && (loop_end == unset || signal.tick > loop_end)) {
            loop_end = signal.tick;
        }
    }

    // Sanity: degenerate loops -- an empty range, or a start sitting on the song's final tick --
    // are dropped entirely; a start with no end runs to the last voice event.
    if (loop_start != unset
        && (loop_start == loop_end || loop_start == last_voice_event_tick)) {
        loop_start = unset;
        loop_end = unset;
    }
    if (loop_start != unset && loop_end == unset) {
        loop_end = last_voice_event_tick;
    }
    if (loop_end != unset && loop_start == unset) {
        // An end alone loops back to the top of the file, which the zero initial value of the
        // reference's loop struct expressed implicitly.
        loop_start = 0;
    }

    Song song;
    song.events = std::move(events);

    if (loop_end != unset && loop_end > loop_start) {
        // Ticks to samples through the tempo map the merge already ordered, so the loop points
        // land on the same grid as the events they sit between.
        std::vector<std::pair<std::int64_t, int>> tempo_changes;
        for (const RawEvent& entry : merged) {
            if (entry.kind == RawEvent::Kind::tempo) {
                tempo_changes.emplace_back(entry.tick, entry.status);
            }
        }
        const auto tick_to_samples = [&](std::int64_t tick) {
            int tempo = default_tempo;
            std::int64_t from = 0;
            double elapsed = 0.0;
            for (const auto& [change_tick, change_tempo] : tempo_changes) {
                if (change_tick >= tick) {
                    break;
                }
                elapsed += static_cast<double>(change_tick - from)
                           * (tempo / 1e6 / ticks_per_quarter);
                from = change_tick;
                tempo = change_tempo;
            }
            elapsed += static_cast<double>(tick - from) * (tempo / 1e6 / ticks_per_quarter);
            return quantise(elapsed * sample_rate);
        };

        SongLoop loop;
        loop.start = tick_to_samples(loop_start);
        loop.end = tick_to_samples(loop_end);
        loop.soft = soft;
        if (loop.end > loop.start) {
            song.loop = loop;
        }
    }

    return song;
}

} // namespace ts::smf
