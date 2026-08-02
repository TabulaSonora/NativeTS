#include "sources.hpp"

#include <algorithm>

namespace ts::player {

void PlaybackBuffer::set_position(std::int64_t frame)
{
    position_ = std::clamp(frame, std::int64_t{0}, length());
}

std::size_t PlaybackBuffer::read(std::span<float> interleaved, float gain)
{
    const auto frames = static_cast<std::int64_t>(interleaved.size() / 2);
    const std::int64_t available = std::clamp(length() - position_, std::int64_t{0}, frames);

    for (std::int64_t i = 0; i < available; ++i) {
        const auto at = static_cast<std::size_t>(position_ + i);
        const auto out = static_cast<std::size_t>(i);
        interleaved[out * 2] = left_[at] * gain;
        interleaved[(out * 2) + 1] = right_[at] * gain;
    }

    // Past the end, keep feeding the device silence rather than stopping abruptly.
    std::fill(
        interleaved.begin() + static_cast<std::ptrdiff_t>(available * 2), interleaved.end(), 0.0F);

    position_ += available;
    return static_cast<std::size_t>(available);
}

StreamingSource::StreamingSource(NoteRenderer& notes,
                                 const ToneGeneratorOptions& options,
                                 const std::filesystem::path& midi,
                                 double tail_seconds)
    : generator_(notes, options), player_(SequencePlayer::from_file(generator_, midi))
{
    length_ = player_.last_event_position()
              + static_cast<std::int64_t>(tail_seconds * ToneGenerator::sample_rate);
}

void StreamingSource::set_position(std::int64_t frame)
{
    player_.seek(std::clamp(frame, std::int64_t{0}, length_));
}

std::size_t StreamingSource::read(std::span<float> interleaved, float gain)
{
    const auto frames = static_cast<std::int64_t>(interleaved.size() / 2);
    const std::int64_t available = std::clamp(length_ - position(), std::int64_t{0}, frames);
    const auto count = static_cast<std::size_t>(available);

    if (left_.size() < count) {
        // Only ever grows, and only on the render thread. The audio callback allocates nothing.
        left_.resize(count);
        right_.resize(count);
    }

    if (count > 0) {
        player_.render(std::span{left_}.first(count), std::span{right_}.first(count));
    }

    for (std::size_t i = 0; i < count; ++i) {
        interleaved[i * 2] = left_[i] * gain;
        interleaved[(i * 2) + 1] = right_[i] * gain;
    }

    std::fill(
        interleaved.begin() + static_cast<std::ptrdiff_t>(count * 2), interleaved.end(), 0.0F);
    return count;
}

} // namespace ts::player
