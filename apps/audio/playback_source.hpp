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
    /// Whether the file addresses this part at all. A mixer lists these and hides the rest.
    bool present = false;

    /// Whether this part is sounding drums *now*.
    ///
    /// Not "is this the drum channel". GS can route any part to the drum path over SysEx and XG
    /// does it from bank select alone, so a mixer that compares the channel number to a configured
    /// drum channel mislabels both directions under XG: a melodic part on channel 10, and drums
    /// anywhere else.
    bool drums = false;

    /// The kit sounding on a drum part, or -1.
    int kit = -1;

    /// The tone map this part's program resolves against, as `ToneMap`.
    ///
    /// Per part and per moment: a bank LSB names a vintage and XG System On moves every part to the
    /// XG map, so one map for the whole mixer is wrong as soon as a file changes mode.
    int map = 0;

    /// The bank the melodic lookup is given, which is not `bank` under XG.
    int lookup_bank = 0;
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
    /// Slots the pool currently holds -- the configured limit, or what a growing pool has reached.
    int voice_capacity = 0;
    /// Whether the pool grows on demand instead of stealing, so the capacity is not a ceiling.
    bool voices_grow = false;
    int note_count = 0;
    int drum_kit = 0;
    /// Whether the engine is in XG mode right now.
    ///
    /// Live rather than configured: a file switches this mid-song, and without it a display
    /// can only be read backwards -- from whether the kit names came out in capitals.
    bool xg_mode = false;
    /// Parts the engine was created with; only the first this many of `parts` are meaningful.
    int part_count = 16;
    std::array<PartSnapshot, 64> parts{};
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

    /// Asks the material to repeat instead of ending.
    ///
    /// Unlike everything else here, this may be called from the controlling thread while the
    /// render thread reads: an implementation honours that with an atomic it applies at its next
    /// block, or ignores the request entirely — the default, and the honest answer for a finished
    /// render that has no player to loop.
    virtual void set_looping(bool looping) noexcept { (void)looping; }

    /// Whether the material is set to repeat.
    [[nodiscard]] virtual bool looping() const noexcept { return false; }

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
