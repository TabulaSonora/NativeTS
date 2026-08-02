#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ts::player {

/// One channel's state, as of the last block rendered.
struct PartSnapshot {
    int program = 0;
    int bank = 0;
    int volume = 0;
    int expression = 0;
    int pan = 0x40;
    /// Voices this channel is currently sounding, including any fading after being stolen.
    int voices = 0;
    bool muted = false;
    bool soloed = false;
};

/// What the engine was doing when the render thread last looked.
///
/// Taken on the render thread and copied to whoever asks, rather than letting a UI thread walk the
/// engine's own state while it is being written. The engine is single-threaded by contract and this
/// is what keeps that true with a mixer attached.
struct EngineSnapshot {
    /// Whether an engine was there to look at; a prerendered buffer has none.
    bool live = false;

    std::int64_t position = 0;
    int active_voices = 0;
    int note_count = 0;
    int drum_kit = 0;
    std::array<PartSnapshot, 16> parts{};
};

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

    /// Describes whatever engine is behind this source, on the render thread.
    ///
    /// The default leaves `live` false, which is the honest answer for a source that is only a
    /// buffer of finished samples: there is no engine to ask, and any mixer over it would be
    /// showing the settings a render already baked in.
    virtual void capture(EngineSnapshot& into) const { into.live = false; }
};

} // namespace ts::player
