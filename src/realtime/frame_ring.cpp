#include "tabulasonora/frame_ring.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace ts {

FrameRing::FrameRing(std::size_t frames)
    : capacity_(std::bit_ceil(std::max<std::size_t>(frames, 2))), mask_(capacity_ - 1)
{
    samples_.resize(capacity_ * 2, 0.0F);
}

void FrameRing::write(std::span<const float> interleaved) noexcept
{
    const std::uint64_t at = write_.load(std::memory_order_relaxed);
    const std::size_t frames = interleaved.size() / 2;

    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t slot = (static_cast<std::size_t>(at) + i) & mask_;
        samples_[slot * 2] = interleaved[i * 2];
        samples_[(slot * 2) + 1] = interleaved[(i * 2) + 1];
    }

    // Release, so the consumer that observes the new counter also observes the samples.
    write_.store(at + frames, std::memory_order_release);
}

std::size_t FrameRing::read(std::span<float> interleaved) noexcept
{
    const std::uint32_t wanted = flush_.load(std::memory_order_acquire);
    if (wanted != ack_.load(std::memory_order_relaxed)) {
        // The producer is waiting on this and is not writing, so `write_` is stable.
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
        ack_.store(wanted, std::memory_order_release);
    }

    const std::uint64_t at = read_.load(std::memory_order_relaxed);
    const std::size_t frames = interleaved.size() / 2;
    const auto available = std::min<std::size_t>(
        frames, static_cast<std::size_t>(write_.load(std::memory_order_acquire) - at));

    float left = 0.0F;
    float right = 0.0F;

    for (std::size_t i = 0; i < available; ++i) {
        const std::size_t slot = (static_cast<std::size_t>(at) + i) & mask_;
        const float l = samples_[slot * 2];
        const float r = samples_[(slot * 2) + 1];

        interleaved[i * 2] = l;
        interleaved[(i * 2) + 1] = r;

        left = std::max(left, std::abs(l));
        right = std::max(right, std::abs(r));
    }

    std::fill(
        interleaved.begin() + static_cast<std::ptrdiff_t>(available * 2), interleaved.end(), 0.0F);

    read_.store(at + available, std::memory_order_release);

    peak_left_.store(left, std::memory_order_relaxed);
    peak_right_.store(right, std::memory_order_relaxed);

    if (available < frames && !expect_starvation_.load(std::memory_order_relaxed)) {
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }

    return available;
}

} // namespace ts
