#include "tabulasonora/voice_pool.hpp"

#include <algorithm>
#include <limits>

namespace ts {

int VoicePool::active_count() const noexcept
{
    return static_cast<int>(std::count_if(
        state_.begin(), state_.end(), [](VoiceState s) { return s != VoiceState::free; }));
}

Voice VoicePool::allocate(int channel, int note, int velocity, int note_group)
{
    int index = find_free();
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
    for (std::size_t i = 0; i < max_voices; ++i) {
        if (state_[i] == VoiceState::held && channel_[i] == channel && note_[i] == note) {
            state_[i] = VoiceState::releasing;
            ++released;
        }
    }
    return released;
}

void VoicePool::reset() noexcept
{
    state_.fill(VoiceState::free);
    counter_ = 0;
    next_note_group_ = 0;
}

std::vector<Voice> VoicePool::active() const
{
    std::vector<Voice> voices;
    for (int i = 0; i < max_voices; ++i) {
        if (state_[static_cast<std::size_t>(i)] != VoiceState::free) {
            voices.push_back(Voice{this, i});
        }
    }
    return voices;
}

int VoicePool::find_free() const noexcept
{
    for (int i = 0; i < max_voices; ++i) {
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
    for (int i = 0; i < max_voices; ++i) {
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

    for (int i = 0; i < max_voices; ++i) {
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
