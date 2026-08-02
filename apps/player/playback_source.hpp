#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ts::player {

/// Something the transport can play: a position, a length, and blocks on demand.
///
/// Two things implement this. `PlaybackBuffer` holds a finished render, which makes seeking free
/// and exact. `StreamingSource` synthesises as it plays, which makes playback start immediately and
/// costs nothing to hold a long song in memory.
///
/// Only the render thread touches a source. The audio callback never sees one -- it reads the ring
/// the render thread fills.
class PlaybackSource {
public:
    PlaybackSource() = default;
    PlaybackSource(const PlaybackSource&) = delete;
    PlaybackSource& operator=(const PlaybackSource&) = delete;
    PlaybackSource(PlaybackSource&&) = delete;
    PlaybackSource& operator=(PlaybackSource&&) = delete;
    virtual ~PlaybackSource() = default;

    /// Sample rate of the material.
    [[nodiscard]] virtual int sample_rate() const noexcept = 0;

    /// Total frames.
    [[nodiscard]] virtual std::int64_t length() const noexcept = 0;

    /// Current play position in frames.
    [[nodiscard]] virtual std::int64_t position() const noexcept = 0;

    /// Moves the play position, clamped to the material.
    virtual void set_position(std::int64_t frame) = 0;

    /// Whether playback has reached the end.
    [[nodiscard]] virtual bool at_end() const noexcept { return position() >= length(); }

    /// Fills an interleaved stereo block from the current position and advances.
    ///
    /// Anything past the end is zero-filled rather than left short, so the device is always handed
    /// a whole block. Returns the frames actually taken from the material.
    virtual std::size_t read(std::span<float> interleaved, float gain) = 0;
};

} // namespace ts::player
