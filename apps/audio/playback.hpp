#pragma once

#include "playback_source.hpp"

#include "tabulasonora/frame_ring.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ts::player {

/// How the output device should be opened.
struct DeviceOptions {
    /// Output device: an index, or a case-insensitive substring of its name. Empty for the default.
    std::string name;

    /// Frames the device asks for at a time.
    int period_frames = 512;

    /// How far ahead of the device the render thread keeps the ring, in milliseconds.
    int latency_ms = 150;

    /// Linear gain applied where blocks are handed over.
    float gain = 1.0F;
};

/// One output device, as the backend describes it.
struct DeviceInfo {
    std::string name;
    bool is_default = false;
};

/// Lists the playback devices the backend can see.
[[nodiscard]] std::vector<DeviceInfo> output_devices();

/// A source, a render thread and an output device, wired together.
///
/// The division of labour is the whole point. The render thread pulls blocks from the source and
/// writes them to a ring; the device's callback copies out of that ring and does nothing else --
/// no allocation, no lock, no engine code. A slow block therefore eats into the ring's lead rather
/// than glitching the device outright.
///
/// Every method here is safe to call from one controlling thread while the other two run. Seeks are
/// queued rather than applied in place, because the render thread owns the source.
class Playback {
public:
    /// Takes ownership of a source and opens a device for it.
    ///
    /// Throws `std::runtime_error` if the backend or the device cannot be opened.
    Playback(std::unique_ptr<PlaybackSource> source, const DeviceOptions& options);

    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    Playback(Playback&&) = delete;
    Playback& operator=(Playback&&) = delete;
    ~Playback();

    /// The device the backend actually opened, and the backend's own name.
    [[nodiscard]] const std::string& device_name() const noexcept;
    [[nodiscard]] const std::string& backend_name() const noexcept;
    [[nodiscard]] int device_rate() const noexcept;

    [[nodiscard]] int sample_rate() const noexcept;
    [[nodiscard]] std::int64_t length() const noexcept;

    /// The position that is currently *audible*, which lags what has been rendered by the ring.
    [[nodiscard]] std::int64_t position() const noexcept;

    /// Whether the source has run out and the ring has drained.
    [[nodiscard]] bool finished() const noexcept;

    [[nodiscard]] bool paused() const noexcept;
    void set_paused(bool paused);

    /// Whether the source repeats instead of ending. Applied by the source at its next block.
    [[nodiscard]] bool looping() const noexcept;
    void set_looping(bool looping) noexcept;

    /// Queues a seek, in frames from the start.
    void seek(std::int64_t frame);

    /// Queues a seek relative to the audible position.
    void seek_by(double seconds);

    void set_gain(float gain) noexcept;
    [[nodiscard]] float gain() const noexcept;

    /// Peak of the last block the device was handed, left and right.
    [[nodiscard]] std::pair<float, float> peak() const noexcept;

    /// How many times the device came up short.
    [[nodiscard]] std::int64_t underruns() const noexcept;

    /// The engine's state as of the render thread's last block.
    [[nodiscard]] EngineSnapshot snapshot() const;

    /// Starts the device, after filling the ring so the first blocks are not counted as underruns.
    void start();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts::player
