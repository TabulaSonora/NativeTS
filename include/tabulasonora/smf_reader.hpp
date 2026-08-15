#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ts {

/// What kind of message an event carries.
enum class MidiEventKind {
    /// A channel voice message.
    channel,
    /// A system-exclusive message, including the leading `F0`.
    sysex,
};

/// One scheduled MIDI message.
struct MidiEvent {
    /// Position in samples, quantised to the render-block grid.
    std::int64_t position = 0;
    /// Which kind of message.
    MidiEventKind kind = MidiEventKind::channel;
    /// Status byte, for a channel message.
    int status = 0;
    /// First data byte.
    int data1 = 0;
    /// Second data byte, or zero for two-byte messages.
    int data2 = 0;
    /// Raw bytes, for a system-exclusive message.
    std::vector<std::uint8_t> sysex;

    /// Which MIDI port the event's track was tagged for.
    ///
    /// A file addresses more than sixteen channels by splitting them across ports, and it says
    /// which port a track belongs to with a meta event: `FF 21` (MIDI Port) carries the number
    /// outright, while `FF 09` (Device Name) — or `FF 04` (Instrument Name) in files that predate
    /// it — names an output instead. One scheme applies per file, preferred in that order: any
    /// `FF 21` anywhere means the numbers rule and the names are ignored, else `FF 09` names,
    /// else `FF 04` names. Names are assigned numbers in order of first appearance, deduplicated
    /// by the string exactly as stored — but a name scheme only applies at all when some MIDI
    /// channel is claimed under more than one distinct name, and only tracks that play exactly
    /// one channel get a vote: a named track spanning several channels is a mix-down riding
    /// along, not a device voice. That collision is the one thing sixteen channels cannot
    /// express alone; without it the names are instrument labels,
    /// and the file is the single-port file it looks like. Untagged files are all port 0, which
    /// is what they have always been. The engine masks this to the ports it was created with, so
    /// a file asking for port 3 on a two-port engine folds onto port 1 rather than being dropped.
    int port = 0;

    /// The channel a channel message addresses.
    [[nodiscard]] int channel() const noexcept { return status & 0x0F; }

    /// The message type nibble.
    [[nodiscard]] int message_type() const noexcept { return status & 0xF0; }
};

/// A dependency-free Standard MIDI File reader.
///
/// Handles formats 0, 1 and 2, running status, system-exclusive messages, and the tempo map. Track
/// ticks are converted to seconds through that map and then to samples, then rounded onto the 1 ms
/// increment the engine recalculates event timing to. Quantising here is what lets an offline
/// render line up with the real engine's own output.
///
/// Meta events other than tempo are consumed here or dropped: port tags pick the port scheme,
/// markers feed the loop scanners, and none of them reach the event list.
namespace smf {

/// Samples per 1 ms — the increment the engine recalculates event timing onto.
///
/// 32 samples is 1 ms at the engine's 32 kHz internal rate, and that is what this is. It was
/// previously called the render-block grid, which it is not: the control block is 320. The value
/// was right and the name was wrong, which is worse than both being wrong, because it invites
/// "correcting" it to 320 -- measured, that collapses short notes badly.
inline constexpr int block_grid = 32;

/// Default tempo when a file sets none: 120 bpm, in microseconds per quarter note.
inline constexpr int default_tempo = 500'000;

/// Loop points a file declared, in samples on the render-block grid.
///
/// Five marker dialects are scanned, ported from spessasynth_core_c (itself from
/// midi_processing's `scan_for_loops`): Touhou's CC 2/CC 4 pair in format-0 files, RPG Maker's
/// CC 111 with a value of zero, LeapFrog's CC 110 begin and CC 111 end, the XMI/EMIDI CC 116–119
/// set, and `loopStart`/`loopEnd` marker meta events. The outermost surviving start and end win. A
/// start with no end runs to the last voice event; a degenerate loop — empty, or starting on the
/// song's final tick — is dropped entirely.
///
/// **Three conventions write CC 110 and CC 111 and they collide**, so which one a file is using has
/// to be settled before either can be read. A CC 112–119 anywhere settles it as EMIDI, since
/// nothing else touches that part of the block — which is why CC 110 and CC 111 must not mark a
/// file as EMIDI themselves. Failing that, the two readings of CC 110 are told apart by *where it
/// sits*: a track designation declares what a track is and is written at its head, while a LeapFrog
/// loop begins inside the song. RPG Maker's reading applies only when no CC 110 has claimed the
/// pair either way.
struct SongLoop {
    /// First sample of the loop body.
    std::int64_t start = 0;
    /// Sample the jump back happens at.
    std::int64_t end = 0;
    /// Whether the file marked the loop end explicitly (a *soft* loop). A soft jump rewinds
    /// without replaying state — anything that must change at the jump is inside the loop body
    /// and re-fires on its own — where a hard loop's end is inferred and the jump replays
    /// controllers the way a seek does.
    bool soft = false;
};

/// A parsed file: the event list plus what the container knew beyond the events.
struct Song {
    /// Every event, ordered by position.
    std::vector<MidiEvent> events;
    /// Loop points, when the file declares any.
    std::optional<SongLoop> loop;

    /// Where the song first sounds, in samples, when that is not the beginning.
    ///
    /// A file often opens with a bar of setup — bank selects, a reset, controllers — and a player
    /// that starts at sample zero plays that silence. This is the **earliest note-on across every
    /// track**, not the first one a track-ordered walk reaches: a file whose first track rests
    /// until the second section would otherwise start there, which for a looping arrangement means
    /// skipping the whole introduction and landing on the loop.
    ///
    /// Zero when the song starts on a note or has none. The events themselves are untouched —
    /// skipping is a playback decision, and `SequencePlayer` is where it is made, so a caller that
    /// wants the lead-in can still have it.
    std::int64_t first_note = 0;
};

/// Reads a music file, with its loop points.
///
/// Not only Standard MIDI Files: the formats `formats::to_smf` recognises — RMID, MIDS, MUS,
/// XMI, GMF, HMP, HMI, XMF, and (by file name) LDS — are converted first, so every caller
/// understands them.
///
/// Files carrying EMIDI track designations (CC 110) are filtered down to one card's copy, since a
/// song authored for several duplicates its content and playing every copy doubles the voices.
/// **The card is chosen from what the file offers**: the Roland Sound Canvas when the file
/// addresses one, since that is what this engine is, and General MIDI otherwise — holding to one
/// card unconditionally would silence every file authored only for the other.
[[nodiscard]] Song load(const std::filesystem::path& path, int sample_rate = 32000);

/// Parses a music file held in memory, with its loop points.
///
/// `name` is the file name the data came from, when the caller knows it; only LDS detection
/// uses it. Throws `std::runtime_error` if the file is malformed or uses SMPTE timing.
[[nodiscard]] Song load(std::span<const std::uint8_t> data,
                        int sample_rate = 32000,
                        std::string_view name = {});

/// Reads a music file, ordered by position — `load` without the loop points.
[[nodiscard]] std::vector<MidiEvent> read(const std::filesystem::path& path,
                                          int sample_rate = 32000);

/// Parses a music file held in memory, ordered by position — `load` without the loop points.
///
/// Throws `std::runtime_error` if the file is malformed or uses SMPTE timing.
[[nodiscard]] std::vector<MidiEvent> parse(std::span<const std::uint8_t> data,
                                           int sample_rate = 32000);

/// Rounds a sample position onto the render-block grid.
///
/// The rounding is to nearest with ties to **even**, matching .NET's `Math.Round`, because that is
/// what the reference build uses. C's `round` and `llround` go half-away-from-zero instead.
///
/// Measured: for *this* expression the choice does not matter. Over 400,000 half-sample positions
/// the two modes disagree at every one of the 100,000 ties and the resulting block is the same
/// every time — the two candidates differ by one, and the floor onto a 32-sample grid absorbs that
/// unless they straddle a multiple of 32, which a tie can never do. `nearbyint` is kept anyway so
/// the expression matches the original exactly rather than by an argument that could stop holding.
///
/// The same is not true of `Math.Round` where no grid follows it — the delay's tap lengths are the
/// case to watch.
[[nodiscard]] std::int64_t quantise(double samples) noexcept;

} // namespace smf
} // namespace ts
