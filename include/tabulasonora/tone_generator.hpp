#pragma once

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/part.hpp"
#include "tabulasonora/sequence_renderer.hpp"
#include "tabulasonora/smf_reader.hpp"
#include "tabulasonora/voice_pool.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace ts {

/// How a running engine should behave.
struct ToneGeneratorOptions {
    /// Which vintage's tone map program changes resolve against.
    ToneMap map = ToneMap::sc8820;

    /// MIDI channel routed to the drum path.
    int drum_channel = 9;

    bool reverb = true;
    bool chorus = true;
    bool delay = true;

    /// Force an effect type instead of taking it from the stream.
    std::optional<int> reverb_type;
    std::optional<int> chorus_type;
    std::optional<int> delay_type;

    /// How long a drum hit rings before its release is spliced in.
    ///
    /// A drum ignores note-off, so this is not the note's length: the tone's own envelope does the
    /// decay and this only bounds how long the voice occupies a slot.
    double drum_ring_seconds = 1.8;

    /// Linear gain applied to the audio handed to the host.
    double output_gain = 1.0;

    /// Per-channel mute and solo, read live so a mixer can change it while sound is running.
    const ChannelMask* channels = nullptr;
};

/// The real-time engine: MIDI in, audio out, rendered a block at a time.
///
/// This is the block-based voice loop the hardware runs. Events are applied at the render-block
/// boundary — the grid the engine itself quantises them to — voices are allocated from a fixed pool
/// of 64 and stolen when it runs out, and each block is summed into a dry pair and three send buses
/// that the effects then process. Nothing about a note has to be known in advance, so a note can be
/// held indefinitely and released whenever.
///
/// It shares its DSP with `SequenceRenderer` rather than reimplementing it: the same envelopes, the
/// same sampler, the same tables. The two differ in what they can express, not in how they sound —
/// the offline path renders each note whole and never runs out of polyphony, while this one
/// enforces the engine's own limit and can be driven live.
///
/// Not thread-safe. Events and rendering must come from the same thread, or be serialised by the
/// caller.
class ToneGenerator {
public:
    /// Internal sample rate.
    static constexpr int sample_rate = NoteRenderer::sample_rate;

    /// Samples per render block — the grid events are applied on.
    static constexpr int block_size = smf::block_grid;

    /// Samples per control tick, at 100 Hz.
    static constexpr int control_block = NoteRenderer::control_block;

    /// Creates an engine over a note renderer's loaded tables, which must outlive it.
    explicit ToneGenerator(NoteRenderer& notes, const ToneGeneratorOptions& options = {});

    ToneGenerator(ToneGenerator&&) noexcept;
    ToneGenerator& operator=(ToneGenerator&&) noexcept;
    ToneGenerator(const ToneGenerator&) = delete;
    ToneGenerator& operator=(const ToneGenerator&) = delete;
    ~ToneGenerator();

    /// Linear gain applied to the audio handed to the host.
    ///
    /// A trim on the way out, applied where the block is copied to the caller rather than inside
    /// the block loop, so no voice, effect or feedback path sees it. `reset` leaves it alone.
    [[nodiscard]] double output_gain() const noexcept;
    void set_output_gain(double gain) noexcept;

    /// How many samples have been rendered since the last reset.
    [[nodiscard]] std::int64_t position() const noexcept;

    /// How many notes have sounded since the last reset.
    ///
    /// A note that resolves to nothing — an unassigned program, or a velocity outside every
    /// partial's window — is not counted, since no voice starts.
    [[nodiscard]] int note_count() const noexcept;

    /// How many voices are currently sounding, including those fading after being stolen.
    [[nodiscard]] int active_voices() const noexcept;

    /// The sixteen parts, indexed by MIDI channel.
    [[nodiscard]] const Part& part(int channel) const noexcept;

    /// The voice allocator.
    [[nodiscard]] const VoicePool& voices() const noexcept;

    /// The drum kit in force, as the last program change on the drum part resolved it.
    ///
    /// Worth reading rather than recomputing: a program the map does not define leaves the kit as
    /// it was, so `kit_for_program` over the part's current program does not always answer what is
    /// actually loaded.
    [[nodiscard]] int drum_kit() const noexcept;

    /// Which drum map row a program change on the drum part resolves against.
    ///
    /// The module derives this from the part's *internal* bank code, and that translation is not
    /// reversed — so nothing in a MIDI file reaches it. Until it is, the row is set by the host,
    /// which is the only way the second map's kits can be sounded at all.
    ///
    /// Deliberately **not** cleared by `reset`. The kit is, because a program change selects it and
    /// `reset` undoes what MIDI did; the row is configuration, like the tone map.
    [[nodiscard]] std::optional<int> drum_map_row() const noexcept;
    void set_drum_map_row(std::optional<int> row) noexcept;

    /// The drum map row this engine actually resolves against.
    [[nodiscard]] int effective_drum_map_row() const noexcept;

    /// Silences everything and returns every part to its power-on state.
    void reset();

    /// Applies one MIDI event; its position is ignored, since it applies now.
    void send(const MidiEvent& message);

    /// Applies one channel voice message.
    void send_channel(int status, int data1, int data2);

    /// Applies one system-exclusive message, including the leading `F0`.
    void send_sysex(std::span<const std::uint8_t> bytes);

    /// Renders audio into two equal-length channels.
    ///
    /// Any length is accepted. Blocks are still rendered whole and the remainder carried, because a
    /// voice counts its control tick in blocks. A caller that wants events to land exactly where
    /// the engine would put them should render in multiples of `block_size` and send between calls.
    ///
    /// Throws `std::invalid_argument` if the two channels differ in length.
    void render(std::span<float> left, std::span<float> right);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts
