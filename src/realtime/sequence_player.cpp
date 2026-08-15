#include "tabulasonora/sequence_player.hpp"

#include <set>

#include "tabulasonora/smf_reader.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ts {

SequencePlayer::SequencePlayer(ToneGenerator& generator, std::vector<MidiEvent> events)
    : generator_(&generator), events_(std::move(events))
{
    last_event_position_ = events_.empty() ? 0 : events_.back().position;
}

SequencePlayer::SequencePlayer(ToneGenerator& generator, smf::Song song)
    : SequencePlayer(generator, std::move(song.events))
{
    loop_ = song.loop;
    first_note_ = song.first_note;
}

void SequencePlayer::skip_lead_in()
{
    // A file that opens with a bar of setup plays as silence until its first note, and a player
    // that starts at sample zero renders that silence. Seeking to it leaves the engine exactly as
    // playing through would: `seek` replays every controller, bank select and SysEx on the way,
    // and only the notes are skipped -- of which there are none before the first one by
    // definition.
    //
    // One sample before the note rather than onto it, matching upstream, so the note is dispatched
    // by the render that follows rather than having already been consumed by the seek.
    if (first_note_ <= 0) {
        return;
    }
    seek(first_note_ - 1);
}

SequencePlayer SequencePlayer::from_file(ToneGenerator& generator,
                                         const std::filesystem::path& path)
{
    return SequencePlayer{generator, smf::load(path, ToneGenerator::sample_rate)};
}

void SequencePlayer::set_loop_count(int count)
{
    loop_count_ = count;
    if (count < 0) {
        // Switching to infinite cancels a pending fade so the song keeps sounding. A fade that
        // already ran to silence has ended the play-through; that is not resurrected.
        if (!finished_) {
            fade_start_ = -1;
        }
        return;
    }
    // A finite target at or below the play-through already reached starts the fade now, without
    // waiting for the next loop-end arrival.
    if (count >= 1 && loops_played_ > count && fade_start_ < 0 && !finished_) {
        fade_start_ = rendered_;
    }
}

void SequencePlayer::set_fade_seconds(double seconds)
{
    fade_seconds_ = std::max(0.0, seconds);
}

void SequencePlayer::all_notes_off()
{
    for (int port = 0; port < generator_->ports(); ++port) {
        for (int channel = 0; channel < Sequence::channel_count; ++channel) {
            generator_->send_channel(port, 0xB0 | channel, 123, 0);
        }
    }
}

void SequencePlayer::dispatch()
{
    dispatch_within(0);
}

void SequencePlayer::dispatch_within(std::int64_t samples)
{
    // Everything falling inside the stretch about to be rendered, each stamped with where it falls
    // relative to that stretch's start.
    //
    // This is the shape the module is driven in: a host hands `TG_ShortMidiIn` a buffer's worth of
    // messages with their offsets and then calls `TG_Process` once, and the engine places them
    // itself. Dispatching on a render boundary instead -- which is what this did, a block at a time
    // -- quantises every event to that boundary before the engine ever sees it, so the engine's own
    // placement can never be observed and any error in it is silently absorbed.
    const std::int64_t limit = position_ + samples;
    while (cursor_ < events_.size() && events_[cursor_].position <= limit) {
        const auto offset = static_cast<int>(
            std::max<std::int64_t>(0, events_[cursor_].position - position_));
        generator_->send_at(offset, events_[cursor_].port, events_[cursor_]);
        ++cursor_;
    }
}

bool SequencePlayer::handle_loop_point()
{
    const bool infinite = loop_count_ < 0;
    const bool fading = fade_start_ >= 0;

    if (loop_ && position_ >= loop_->end) {
        // Arrived at the loop end. Whether to jump follows the reference sequencer: infinite
        // always jumps; a finite target of two or more jumps too, starting the fade the moment
        // the target pass count is reached -- and keeps jumping through the fade, so the music
        // sounds all the way down instead of trailing into silence past the final pass. A count
        // of zero or one plays straight through.
        if (!infinite && !fading && loop_count_ < 2) {
            return false;
        }
        ++loops_played_;
        if (!infinite && fade_start_ < 0 && loops_played_ >= loop_count_) {
            fade_start_ = rendered_;
        }

        if (loop_->soft) {
            // The file marked this end itself: everything the jump must change is inside the
            // loop body. Kill hanging notes and rewind.
            all_notes_off();
            position_ = loop_->start;
            cursor_ = static_cast<std::size_t>(
                std::lower_bound(events_.begin(),
                                 events_.end(),
                                 loop_->start,
                                 [](const MidiEvent& event, std::int64_t target) {
                                     return event.position < target;
                                 })
                - events_.begin());
        } else {
            // The end was inferred, so nothing in the body re-establishes state: replay it the
            // way a seek does.
            replay_to(loop_->start);
        }
        return true;
    }

    // No markers: infinite looping rewinds the whole file once everything has been dispatched
    // and passed.
    if (infinite && !loop_ && !events_.empty() && cursor_ >= events_.size()
        && position_ >= last_event_position_) {
        ++loops_played_;
        replay_to(0);
        return true;
    }

    return false;
}

void SequencePlayer::render(std::span<float> left, std::span<float> right)
{
    if (left.size() != right.size()) {
        throw std::invalid_argument("The two channels must be the same length.");
    }

    for (std::size_t start = 0; start < left.size();) {
        if (finished_) {
            const auto rest = left.size() - start;
            std::fill_n(left.begin() + static_cast<std::ptrdiff_t>(start), rest, 0.0F);
            std::fill_n(right.begin() + static_cast<std::ptrdiff_t>(start), rest, 0.0F);
            break;
        }

        // Whatever is already due, then the loop check, then whatever is due after a jump -- the
        // order this has always had, and the reason an event sitting exactly on the loop end still
        // reaches the engine before the jump takes the cursor away from it.
        dispatch_within(0);
        if (handle_loop_point()) {
            dispatch_within(0);
        }

        // How far this pass may run, measured *after* the jump, because a jump moves the position
        // the bound is taken from. Getting that order wrong lets a pass begin at the loop end, find
        // nothing to bound it, and run to the end of the buffer -- overshooting the loop instead of
        // landing on it.
        //
        // Not a fixed block: events are handed over with the offset they fall at and the engine
        // stamps that to the millisecond, and a 32-sample pass is under one, so everything would
        // round to the top of it and the engine's own placement would never be exercised. The loop
        // end is the bound because it is the one position that has to land exactly.
        auto count = left.size() - start;
        if (loop_ && position_ < loop_->end) {
            count = std::min<std::size_t>(count,
                                          static_cast<std::size_t>(loop_->end - position_));
        }
        count = std::max<std::size_t>(count, 1);

        dispatch_within(static_cast<std::int64_t>(count) - 1);

        generator_->render(left.subspan(start, count), right.subspan(start, count));

        if (fade_start_ >= 0) {
            // The post-loop fade: a linear ramp on the monotonic render clock, immune to the
            // position jumping backward under it.
            const double fade_samples = fade_seconds_ * ToneGenerator::sample_rate;
            for (std::size_t i = 0; i < count; ++i) {
                const double elapsed = static_cast<double>(rendered_ - fade_start_)
                                       + static_cast<double>(i);
                const double gain =
                    fade_samples <= 0.0 ? 0.0 : std::max(0.0, 1.0 - (elapsed / fade_samples));
                left[start + i] *= static_cast<float>(gain);
                right[start + i] *= static_cast<float>(gain);
            }
            if (static_cast<double>(rendered_ + static_cast<std::int64_t>(count) - fade_start_)
                >= fade_samples) {
                finished_ = true;
            }
        }

        position_ += static_cast<std::int64_t>(count);
        rendered_ += static_cast<std::int64_t>(count);
        start += count;
    }
}

RenderResult SequencePlayer::render_to_end(double tail_seconds, std::optional<double> end_seconds)
{
    std::int64_t end = last_event_position_;

    // A finite loop target stretches the render over the passes and the fade. The first pass
    // reaches the loop end once, every further arrival adds one body length, and the fade starts
    // on arrival number `loop_count_`. An infinite target cannot be rendered to a buffer and is
    // ignored here.
    if (loop_ && loop_count_ >= 2) {
        const std::int64_t body = loop_->end - loop_->start;
        const auto fade =
            static_cast<std::int64_t>(fade_seconds_ * ToneGenerator::sample_rate);
        end = loop_->end + (static_cast<std::int64_t>(loop_count_) - 1) * body + fade;
    }

    if (end_seconds) {
        end = std::min(end, static_cast<std::int64_t>(*end_seconds * ToneGenerator::sample_rate));
    }

    const auto total = static_cast<std::int64_t>(static_cast<double>(end)
                                                 + (tail_seconds * ToneGenerator::sample_rate));
    if (total <= 0) {
        return RenderResult{};
    }

    RenderResult result;
    result.left.assign(static_cast<std::size_t>(total), 0.0F);
    result.right.assign(static_cast<std::size_t>(total), 0.0F);
    render(result.left, result.right);

    float peak = 0.0F;
    for (std::size_t i = 0; i < result.left.size(); ++i) {
        peak = std::max({peak, std::abs(result.left[i]), std::abs(result.right[i])});
    }

    result.sample_rate = ToneGenerator::sample_rate;
    result.note_count = notes_before_reset_ + generator_->note_count();
    result.peak = peak;
    return result;
}

std::vector<int> SequencePlayer::addressed_parts() const
{
    std::set<int> seen;
    for (const MidiEvent& event : events_) {
        if (event.kind != MidiEventKind::channel) {
            continue;
        }
        // Note-offs and all-notes-off alone do not make a part present -- a file that only ever
        // silences a channel is not using it.
        const int type = event.message_type();
        if (type == 0x80) {
            continue;
        }
        if (type == 0x90 && event.data2 == 0) {
            continue;
        }
        seen.insert((event.port * Sequence::channel_count) + event.channel());
    }
    return {seen.begin(), seen.end()};
}

void SequencePlayer::replay_to(std::int64_t sample)
{
    const std::int64_t target = std::max<std::int64_t>(0, sample);

    // The reset also clears the engine's note count, and the notes it counted did render --
    // fold them in, so a looped render reports the whole performance rather than the last pass.
    notes_before_reset_ += generator_->note_count();
    generator_->reset();
    cursor_ = 0;
    position_ = 0;

    while (cursor_ < events_.size() && events_[cursor_].position < target) {
        const MidiEvent& event = events_[cursor_];
        ++cursor_;

        // Notes are what a seek skips. Everything else is state the engine would be carrying.
        const int type = event.kind == MidiEventKind::channel ? event.message_type() : 0;
        if (type == 0x80 || type == 0x90 || type == 0xA0) {
            continue;
        }

        generator_->send(event.port, event);
    }

    position_ = target;
}

void SequencePlayer::seek(std::int64_t sample)
{
    // A manual seek cancels any active fade and restarts the loop counter, matching the
    // reference sequencer.
    fade_start_ = -1;
    finished_ = false;
    loops_played_ = 0;
    replay_to(sample);
}

} // namespace ts
