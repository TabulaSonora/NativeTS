#pragma once

#include "tabulasonora/render_options.hpp"
#include "tabulasonora/smf_reader.hpp"
#include "tabulasonora/tone_generator.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace ts {

/// Drives a running engine from a MIDI event list.
///
/// The engine itself has no notion of a file: this is what turns one into the stream of events it
/// consumes, dispatching each at the render block it falls in.
class SequencePlayer {
public:
    /// Creates a player over an engine and an event list ordered by position.
    ///
    /// Both must outlive the player.
    SequencePlayer(ToneGenerator& generator, std::vector<MidiEvent> events);

    /// Creates a player over an engine and a parsed song, keeping its loop points.
    ///
    /// The loop points only matter once `set_loop_count` asks for looping; the default remains a
    /// single straight play-through.
    SequencePlayer(ToneGenerator& generator, smf::Song song);

    /// Reads a music file and creates a player for it.
    ///
    /// Every format `smf::load` handles, loop points included.
    [[nodiscard]] static SequencePlayer from_file(ToneGenerator& generator,
                                                  const std::filesystem::path& path);

    /// Which parts the file actually addresses, as `port * 16 + channel`.
    ///
    /// A mixer wants the channels a file uses, not sixteen rows of which four are silent -- and a
    /// four-port file has sixty-four to choose from, so showing them all would be worse than
    /// useless. Computed once from the event list, so it is what the file contains rather than what
    /// has been reached so far.
    [[nodiscard]] std::vector<int> addressed_parts() const;

    /// The engine being driven.
    [[nodiscard]] ToneGenerator& generator() noexcept { return *generator_; }

    /// Position of the final event of any kind.
    ///
    /// Not the same as the last note: a file commonly closes with controller or meta traffic after
    /// the music stops, and stopping at the last note clips the tail.
    [[nodiscard]] std::int64_t last_event_position() const noexcept { return last_event_position_; }

    /// Current position in samples.
    [[nodiscard]] std::int64_t position() const noexcept { return position_; }

    /// The file's loop points, when it declared any.
    [[nodiscard]] const std::optional<smf::SongLoop>& loop() const noexcept { return loop_; }

    /// How many times the looped section should play, counting the first pass.
    ///
    /// The vocabulary is the reference sequencer's: `0` or `1` plays the song once straight
    /// through, which is the default; `-1` loops forever; `N >= 2` targets `N` play-throughs of
    /// the loop body, after which the music keeps looping under a fade to silence rather than
    /// trailing off mid-phrase. A file with no loop points loops whole under `-1` and plays
    /// straight through under any finite count. Asking for a count at or below the play-through
    /// already reached starts the fade immediately; switching to `-1` cancels a pending fade.
    void set_loop_count(int count);

    [[nodiscard]] int loop_count() const noexcept { return loop_count_; }

    /// Completed passes over the loop body, counting up from zero on the initial pass.
    [[nodiscard]] int loops_played() const noexcept { return loops_played_; }

    /// The post-loop fade length. Zero cuts at the loop end.
    void set_fade_seconds(double seconds);

    /// Whether every event has been dispatched and nothing is still sounding, or the post-loop
    /// fade has run to silence.
    [[nodiscard]] bool at_end() const noexcept
    {
        return finished_ || (cursor_ >= events_.size() && generator_->active_voices() == 0);
    }

    /// Renders audio, dispatching every event that falls inside it.
    ///
    /// When looping is engaged this is also where the jumps happen: a *soft* loop -- one whose
    /// end the file marked explicitly -- rewinds with an all-notes-off and nothing else, because
    /// whatever must change at the jump is written inside the loop body and re-fires on its own;
    /// a *hard* loop's end was inferred, so the jump replays state the way a seek does.
    ///
    /// Throws `std::invalid_argument` if the two channels differ in length.
    void render(std::span<float> left, std::span<float> right);

    /// Streams the whole file into memory, from wherever the player currently is.
    ///
    /// The length is computed the same way the offline renderer computes it, so the two line up
    /// sample for sample. A finite loop count extends it to cover the passes and the fade; an
    /// infinite one is ignored here, because a buffer cannot hold forever.
    [[nodiscard]] RenderResult render_to_end(double tail_seconds = 2.2,
                                             std::optional<double> end_seconds = std::nullopt);

    /// Jumps to a position, leaving the engine in the state the file would have put it in.
    ///
    /// Every event up to that point is replayed except the notes themselves, so program changes,
    /// bank selects, controllers and the GS effect selections all arrive — a seek into the middle
    /// of a song sounds the way playing up to that point would, without the notes in between.
    ///
    /// A manual seek also cancels any post-loop fade and restarts the loop counter.
    void seek(std::int64_t sample);

    /// Skips the silent lead-in, landing one sample before the song's first note.
    ///
    /// Opt-in rather than automatic, because it is a playback decision and not every caller wants
    /// it: a gate comparing against a reference render must start where the reference started, and
    /// an offline render whose output is spliced elsewhere may want the file's own timing. The
    /// players call it; the oracle gates do not.
    ///
    /// Nothing is lost by skipping. It goes through `seek`, so every controller, bank select and
    /// SysEx in the lead-in is still replayed into the engine — the only events dropped are notes,
    /// and before the first note there are none. A no-op for a song that starts on a note or has
    /// none at all.
    void skip_lead_in();

    /// Where the song's first note falls, in samples, or zero when it starts on one.
    [[nodiscard]] std::int64_t first_note() const noexcept { return first_note_; }

    /// Hands a dense opening over at a cable's rate instead of all at once.
    ///
    /// **Off by default, on in the players**, for the same reason as `skip_lead_in`: a render is
    /// data measured against a reference, and the reference is the module fed the way a host feeds
    /// it. A player is standing in for the cable.
    ///
    /// The engine drops whatever a caller hands it past the input queue's 2,048 packets in one
    /// control tick, and that is faithful — it is what the module does to a host that dumps a burst
    /// on it. But MIDI is 31,250 baud, so `darkness3.mid`'s opening of about 660 bytes takes
    /// roughly 210 ms to arrive over a wire, twenty-one control ticks, and hardware drops none of
    /// it. Handing it over in one call is this player's choice, not the file's.
    ///
    /// With this on, a burst past the budget spills into the following calls. Measured on
    /// `darkness3.mid` through the same change in the oracle harness: its parts stop keeping the
    /// bulk dump's programs and take the file's own, and the render moves 2.31 dB.
    void set_spread_bursts(bool spread) noexcept { spread_bursts_ = spread; }

    [[nodiscard]] bool spread_bursts() const noexcept { return spread_bursts_; }

private:
    /// The seek body: replays state up to `sample` without touching the loop bookkeeping.
    void replay_to(std::int64_t sample);

    /// Silences every part the engine has, for the soft-loop rewind.
    void all_notes_off();

    /// Dispatches every pending event at the current position.
    void dispatch();

    /// Dispatches everything inside the next `samples`, stamped by where each event falls.
    void dispatch_within(std::int64_t samples);

    /// Handles a loop-end arrival: counts the pass, starts the fade when the target is reached,
    /// and jumps. Returns whether a jump happened.
    bool handle_loop_point();

    ToneGenerator* generator_;
    std::vector<MidiEvent> events_;
    std::int64_t last_event_position_ = 0;
    std::int64_t position_ = 0;
    std::size_t cursor_ = 0;

    std::optional<smf::SongLoop> loop_;
    std::int64_t first_note_ = 0;
    bool spread_bursts_ = false;

    /// Under the engine's 2,048 so a message straddling the edge still fits.
    static constexpr int spread_packet_budget = 2000;

    /// The rate the engine's input queue drains at: `TG_Process` empties it once per control tick.
    static constexpr int control_tick_samples = 320;

    /// Whether the last dispatch held part of a burst back for the next pass.
    bool burst_held_ = false;

    /// Packets handed over in the current pass, across both of its dispatch calls.
    int pass_packets_ = 0;
    int loop_count_ = 1;
    int loops_played_ = 0;
    double fade_seconds_ = 7.0;
    /// Total samples ever rendered. Monotonic where `position_` jumps backward at a loop, which
    /// is what makes it the clock the fade can run on.
    std::int64_t rendered_ = 0;
    /// The `rendered_` time the fade began, or negative for no fade.
    std::int64_t fade_start_ = -1;
    bool finished_ = false;
    /// Notes the engine counted before a replay reset it, so a looped render reports all of them.
    int notes_before_reset_ = 0;
};

} // namespace ts
