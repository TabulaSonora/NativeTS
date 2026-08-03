#include "tabulasonora/voice_pool.hpp"

#include <algorithm>
#include <limits>

namespace ts {

VoicePool::VoicePool(int polyphony, bool growing)
    : capacity_(std::max(1, polyphony)), high_water_(std::max(1, polyphony)), growing_(growing)
{
    state_.assign(static_cast<std::size_t>(capacity_), VoiceState::free);
    channel_.assign(static_cast<std::size_t>(capacity_), 0);
    note_.assign(static_cast<std::size_t>(capacity_), 0);
    velocity_.assign(static_cast<std::size_t>(capacity_), 0);
    note_group_.assign(static_cast<std::size_t>(capacity_), 0);
    sequence_.assign(static_cast<std::size_t>(capacity_), 0);
}

int VoicePool::grow()
{
    const int first = capacity_;
    capacity_ += growth_chunk;
    high_water_ = std::max(high_water_, capacity_);

    const auto size = static_cast<std::size_t>(capacity_);
    state_.resize(size, VoiceState::free);
    channel_.resize(size, 0);
    note_.resize(size, 0);
    velocity_.resize(size, 0);
    note_group_.resize(size, 0);
    sequence_.resize(size, 0);
    return first;
}

int VoicePool::active_count() const noexcept
{
    return static_cast<int>(std::count_if(
        state_.begin(), state_.end(), [](VoiceState s) { return s != VoiceState::free; }));
}

Voice VoicePool::allocate(int channel, int note, int velocity, int note_group)
{
    int index = find_free();

    // A growing pool takes the branch a stealing one would have: rather than choosing a victim, it
    // makes room. Every note in the file sounds, at the cost of an unbounded slot count.
    if (index < 0 && growing_) {
        index = grow();
    }

    if (index < 0) {
        index = find_oldest(VoiceState::releasing);
    }
    if (index < 0) {
        index = find_oldest(VoiceState::held);
    }
    if (index < 0) {
        index = 0;
    }

    const auto slot = static_cast<std::size_t>(index);
    if (state_[slot] != VoiceState::free) {
        // Take the whole note, not half of it: a surviving partial of a stolen note would keep
        // sounding on its own.
        steal_group(note_group_[slot], index);
        if (stealing) {
            stealing(index);
        }
    }

    state_[slot] = VoiceState::held;
    channel_[slot] = channel;
    note_[slot] = note;
    velocity_[slot] = velocity;
    note_group_[slot] = note_group;
    sequence_[slot] = ++counter_;

    return Voice{this, index};
}

int VoicePool::release(int channel, int note) noexcept
{
    int released = 0;
    for (std::size_t i = 0; i < state_.size(); ++i) {
        if (state_[i] == VoiceState::held && channel_[i] == channel && note_[i] == note) {
            state_[i] = VoiceState::releasing;
            ++released;
        }
    }
    return released;
}

void VoicePool::reset() noexcept
{
    std::fill(state_.begin(), state_.end(), VoiceState::free);
    counter_ = 0;
    next_note_group_ = 0;
}

std::vector<Voice> VoicePool::active() const
{
    std::vector<Voice> voices;
    for (int i = 0; i < capacity_; ++i) {
        if (state_[static_cast<std::size_t>(i)] != VoiceState::free) {
            voices.push_back(Voice{this, i});
        }
    }
    return voices;
}

int VoicePool::find_free() const noexcept
{
    for (int i = 0; i < capacity_; ++i) {
        if (state_[static_cast<std::size_t>(i)] == VoiceState::free) {
            return i;
        }
    }
    return -1;
}

int VoicePool::find_oldest(VoiceState state) const noexcept
{
    int best = -1;
    std::int64_t oldest = std::numeric_limits<std::int64_t>::max();
    for (int i = 0; i < capacity_; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        if (state_[slot] == state && sequence_[slot] < oldest) {
            oldest = sequence_[slot];
            best = i;
        }
    }
    return best;
}

void VoicePool::steal_group(int note_group, int except)
{
    if (note_group == 0) {
        return;
    }

    for (int i = 0; i < capacity_; ++i) {
        const auto slot = static_cast<std::size_t>(i);
        if (i != except && note_group_[slot] == note_group && state_[slot] != VoiceState::free) {
            state_[slot] = VoiceState::free;
            if (stealing) {
                stealing(i);
            }
        }
    }
}

} // namespace ts
