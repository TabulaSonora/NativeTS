#include "tabulasonora/sequence_player.hpp"

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

SequencePlayer SequencePlayer::from_file(ToneGenerator& generator,
                                         const std::filesystem::path& path)
{
    return SequencePlayer{generator, smf::read(path, ToneGenerator::sample_rate)};
}

void SequencePlayer::render(std::span<float> left, std::span<float> right)
{
    if (left.size() != right.size()) {
        throw std::invalid_argument("The two channels must be the same length.");
    }

    for (std::size_t start = 0; start < left.size(); start += ToneGenerator::block_size) {
        const auto count = std::min<std::size_t>(ToneGenerator::block_size, left.size() - start);

        // Events land on the block boundary, which is the grid the engine itself quantises them to.
        while (cursor_ < events_.size() && events_[cursor_].position <= position_) {
            generator_->send(events_[cursor_]);
            ++cursor_;
        }

        generator_->render(left.subspan(start, count), right.subspan(start, count));
        position_ += static_cast<std::int64_t>(count);
    }
}

RenderResult SequencePlayer::render_to_end(double tail_seconds, std::optional<double> end_seconds)
{
    std::int64_t end = last_event_position_;
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
    result.note_count = generator_->note_count();
    result.peak = peak;
    return result;
}

void SequencePlayer::seek(std::int64_t sample)
{
    const std::int64_t target = std::max<std::int64_t>(0, sample);

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

        generator_->send(event);
    }

    position_ = target;
}

} // namespace ts
