#include "tabulasonora/render_options.hpp"

#include <algorithm>

namespace ts {

bool ChannelMask::any_soloed() const noexcept
{
    return std::any_of(soloed_.begin(), soloed_.end(), [](const std::atomic<bool>& s) {
        return s.load(std::memory_order_relaxed);
    });
}

bool ChannelMask::is_audible(int channel) const noexcept
{
    if (channel < 0 || channel >= channel_count) {
        return false;
    }
    // Solo wins outright: once anything is soloed, a mute on a soloed channel does not silence it.
    if (any_soloed()) {
        return soloed_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
    }
    return !muted_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
}

bool ChannelMask::is_muted(int channel) const noexcept
{
    return channel >= 0 && channel < channel_count
           && muted_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
}

bool ChannelMask::is_soloed(int channel) const noexcept
{
    return channel >= 0 && channel < channel_count
           && soloed_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed);
}

void ChannelMask::set_muted(int channel, bool muted) noexcept
{
    if (channel >= 0 && channel < channel_count) {
        muted_[static_cast<std::size_t>(channel)].store(muted, std::memory_order_relaxed);
    }
}

void ChannelMask::set_soloed(int channel, bool soloed) noexcept
{
    if (channel >= 0 && channel < channel_count) {
        soloed_[static_cast<std::size_t>(channel)].store(soloed, std::memory_order_relaxed);
    }
}

void ChannelMask::reset() noexcept
{
    for (std::size_t i = 0; i < channel_count; ++i) {
        muted_[i].store(false, std::memory_order_relaxed);
        soloed_[i].store(false, std::memory_order_relaxed);
    }
}

bool ChannelMask::is_default() const noexcept
{
    return std::none_of(
               muted_.begin(),
               muted_.end(),
               [](const std::atomic<bool>& m) { return m.load(std::memory_order_relaxed); })
           && !any_soloed();
}

} // namespace ts
