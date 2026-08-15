#include "tabulasonora/tone_generator.hpp"

#include "dsp/simd.hpp"
#include "realtime/partial_voice.hpp"
#include "tabulasonora/control_decode.hpp"
#include "tabulasonora/effect_programmer.hpp"
#include "tabulasonora/equalizer.hpp"
#include "tabulasonora/insertion_effect.hpp"
#include "tabulasonora/send_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace ts {
namespace {

/// One row of an XG effect-type translation: the XG type pair, and the macro this engine runs.
struct XgEffectMacro {
    int msb;
    int lsb;
    int macro;
};

/// XG reverb types the module can express, and nothing else.
///
/// Read out of its translation table rather than chosen. What is *absent* is the informative part:
/// ROOM3, STAGE1, STAGE2 and PLATE have no row, so a file asking for them keeps whatever reverb was
/// already running. Approximating them would be a plausible-sounding departure from the module.
constexpr std::array<XgEffectMacro, 12> xg_reverb_macros{{
    {1, 0, 3},  {1, 1, 4},  {2, 0, 0},  {2, 1, 1},  {2, 2, 2},   {3, 0, 3},
    {3, 1, 3},  {4, 0, 5},  {16, 0, 0}, {17, 0, 5}, {18, 0, 0},  {19, 0, 0},
}};

/// XG chorus types, same shape. CELESTE 1-3 and the flangers are likewise absent.
constexpr std::array<XgEffectMacro, 14> xg_chorus_macros{{
    {65, 0, 0}, {65, 1, 0}, {65, 2, 2}, {65, 8, 0}, {66, 0, 0}, {66, 1, 0}, {66, 2, 2},
    {66, 8, 0}, {67, 0, 5}, {67, 1, 5}, {67, 8, 5}, {68, 0, 0}, {72, 0, 0}, {87, 0, 0},
}};

[[nodiscard]] std::optional<int> xg_macro_lookup(std::span<const XgEffectMacro> table, int msb,
                                                 int lsb) noexcept
{
    for (const XgEffectMacro& row : table) {
        if (row.msb == msb && row.lsb == lsb) {
            return row.macro;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<int> xg_reverb_macro(int msb, int lsb) noexcept
{
    return xg_macro_lookup(xg_reverb_macros, msb, lsb);
}

[[nodiscard]] std::optional<int> xg_chorus_macro(int msb, int lsb) noexcept
{
    return xg_macro_lookup(xg_chorus_macros, msb, lsb);
}

} // namespace

/// Defined below, beside the GS path that was its only caller until XG arrived.
[[nodiscard]] bool apply_control_update(Part& part, const ControlUpdate& update);

struct ToneGenerator::Impl {
    Impl(NoteRenderer& renderer, const ToneGeneratorOptions& opts) : notes(&renderer), options(opts)
    {
        // The part index is formed by masking the port field, so the count has to be a power of
        // two. Anything else would silently alias one port onto another, which is worse than
        // refusing it.
        if (opts.ports != 1 && opts.ports != 2 && opts.ports != ToneGenerator::max_port_count) {
            throw std::invalid_argument(
                "ToneGeneratorOptions::ports must be 1, 2 or "
                + std::to_string(ToneGenerator::max_port_count) + ", not "
                + std::to_string(opts.ports));
        }
        port_count = opts.ports;

        // Configuring the XG map *is* configuring XG mode: on the module one implies the other,
        // and a host that asks for the XG map without the parser to match would get XG presets
        // addressed by GS bank select, which is a state no module has.
        xg_mode = opts.map == ToneMap::xg;

        output_gain = opts.output_gain;
        reverb_type = opts.reverb_type;
        chorus_type = opts.chorus_type;
        delay_type = opts.delay_type;

        pool = VoicePool{opts.polyphony == ToneGeneratorOptions::unlimited_polyphony
                             ? VoicePool::default_polyphony
                             : opts.polyphony,
                         opts.polyphony == ToneGeneratorOptions::unlimited_polyphony};
        pool.stealing = [this](int index) { steal(index); };
        slots.resize(static_cast<std::size_t>(pool.capacity()));

        parts.resize(static_cast<std::size_t>(port_count * Sequence::channel_count));
        standing_lfo.resize(parts.size());
        // One buffer per kit slot. `kit_slot` is `(port, map)` in GS and one per part in XG, so
        // sizing this like `parts` covers both without the index needing a second rule.
        drum_setup.resize(parts.size());
        // One per part, which is what XG needs; GS uses only the first `ports * 2` of them.
        // The two indexings overlap, and may because every mode change resets the array.
        drum_kit.assign(parts.size(), 0);
        note_batch_chunk.assign(parts.size(), -1);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            parts[i].rx_channel = static_cast<int>(i) % Sequence::channel_count;
        }

        // The two tables that pick the anti-zipper hold, finally read. The volume path's rate word
        // is a constant at the call site, so its mask is a constant too -- resolved once here
        // rather than per voice per block.
        volume_ramp_mask = ControlRamp::mask_of(ControlRamp::volume_rate_word,
                                                renderer.tables().ramp_flagword(),
                                                renderer.tables().ramp_divider());
    }

    NoteRenderer* notes;
    ToneGeneratorOptions options;

    /// Ports this engine runs, always a power of two so `part_of` can mask.
    int port_count = ToneGenerator::port_count;

    VoicePool pool;
    std::vector<Part> parts;

    /// The LFO nodes that stand between program changes, one set per part.
    ///
    /// The module's pool keeps a per-part descriptor with a slot for LFO1 and one per partial index
    /// for LFO2, and a node whose waveform byte has bit 5 set inherits whatever stands there rather
    /// than initialising -- taking no generator draw with it. Measured with `scdec lfonodes`: the
    /// slot survives the voice, so notes seconds apart still inherit, and only a program change
    /// empties it. See `LfoConfig::shares_node`.
    struct StandingLfoNodes {
        std::shared_ptr<LfoRunner> lfo1;
        std::array<std::shared_ptr<LfoRunner>, 2> lfo2{};

        void clear() noexcept
        {
            lfo1.reset();
            for (std::shared_ptr<LfoRunner>& node : lfo2) {
                node.reset();
            }
        }
    };
    std::vector<StandingLfoNodes> standing_lfo;

    /// The drum-setup planes, **one buffer per kit slot rather than one per part**.
    ///
    /// This is the module's arrangement and the difference is measurable. A drum program change
    /// reseeds its buffer from the kit record -- unconditionally, even when it selects the kit
    /// already in force -- but it reseeds **only the buffer its own part is using**. Written into
    /// the other slot, a drum setup survives every program change indefinitely:
    ///
    ///     slot 0:  write 30 -> prog kit 0 -> 64   -> write 30 -> prog kit 24 -> 64
    ///     slot 1:  write 30 -> prog kit 0 -> 30   -> write 30 -> prog kit 24 -> 30
    ///
    /// Which matters because a `49` bulk dump writes **both** slots, so half of what it carries is
    /// beyond the reach of any program change and comes into play the moment a part switches slot
    /// -- GS's "Use for Rhythm Part", 1 or 2, which the `48` patch dump sets. Holding these on the
    /// `Part` made that inexpressible: a program change cleared whatever the part held, slot and
    /// all, including data the module keeps.
    ///
    /// Indexed by `kit_slot`, which is already the module's `(port, map)` pair in GS and widens to
    /// one per part in XG for the reason given there.
    std::vector<DrumKeyOverrides> drum_setup;

    /// One voice of a note that has been allocated but whose parameters have not been read yet.
    ///
    /// The module splits a note-on in two. `note_assign_poly` runs in the note-on handler, at
    /// dispatch, in the order the messages arrived: it allocates the group, steals if it must, and
    /// hangs the note off the part at +0x270. Nothing reads the note's parameters there. That waits
    /// for `tg_start_pending_voices @ 18008f020` at the top of the chunk, which walks the **part
    /// array by index** -- indexed by GS *block*, so part 10 first, then channels 1-9, then 11-16.
    ///
    /// Everything draw-free is built at dispatch and kept here; the three fields the shared
    /// generator feeds -- the base pitch's jitter, the pitch envelope's start, and the random pan --
    /// are filled in by the block-ordered pass, the only place they can be drawn in the module's
    /// order.
    struct PendingVoice {
        int slot = -1;
        VoiceSetup setup;
        int key = 0;
        int velocity = 0;
        /// Melodic: the base pitch before its random offset and before the engine's clamp at zero.
        int base_pitch_raw = 0;
        double scale_offset = 0.0;
        /// Drum: the plane-derived pitch before the same offset, and the sample's own rate.
        int drum_pitch_raw = 0;
        double native = 0.0;
    };

    struct PendingNote {
        int part = 0;
        int note = 0;
        bool random_pan = false;

        /// Set when the note-off arrives before the setup pass has reached this note.
        ///
        /// A zero-length note -- note-on and note-off at the same tick -- is not a curiosity, it is
        /// ordinary sequencer output: `shangai.mid` has four on channel 1 alone. The module handles
        /// it because `note_assign_poly` hangs the group off the part at dispatch, so the note-off
        /// finds it there whether or not its parameters have been read yet. This split, which only
        /// exists so the random draws land in the module's order, put the voice somewhere a
        /// note-off could not see: `stop_note` scans `slots`, and the slot is still empty.
        ///
        /// The consequence was not a clipped note but a stuck one -- the voice started a tick later
        /// with nothing left to release it and sounded until the song ended. Measured against the
        /// module on program 77: it starts the note and releases it at once, down from 0.0033 to
        /// the 1e-4 floor within 60 ms, while this port held it at full sustain for the remaining
        /// 1.5 seconds of the probe.
        bool released = false;
        /// All Sound Off cuts rather than releasing, and reaches these the same way.
        bool choke = false;
        int release_damper = 0;

        std::vector<PendingVoice> voices;
    };

    std::vector<PendingNote> pending_notes;

    /// The order `tg_start_pending_voices` visits parts in: by port, then by GS block number.
    [[nodiscard]] static int block_order_key(int part, int channels) noexcept
    {
        const int port = part / channels;
        const int channel = part % channels;
        const int block = channel == 9 ? 0 : (channel < 9 ? channel + 1 : channel);
        return (port * channels) + block;
    }

    /// Reads the parameters of everything allocated this chunk, part by part in block order.
    void start_pending_voices();

    /// The chunk each part last opened a note-on batch on, or -1.
    ///
    /// Unused: `seed_part_lfo_nodes` gets the same batching from the sorted run of equal parts,
    /// without a side table to keep in step with the pool's size. Kept only until something needs a
    /// per-part-per-chunk gate again.
    std::vector<std::int64_t> note_batch_chunk;

    /// One voice per slot, or empty. Recycled through `spare` rather than reallocated per note.
    /// One voice per slot, sized to the pool. A growing pool resizes this alongside itself.
    std::vector<std::unique_ptr<PartialVoice>> slots;
    std::vector<std::unique_ptr<PartialVoice>> spare;
    /// Voices taken from their slot by stealing, still fading out.
    std::vector<std::unique_ptr<PartialVoice>> dying;

    std::array<float, block_size> scratch{};

    /// The voice's part-volume gain, one entry a sample, refilled by its anti-zipper ramp.
    std::array<double, block_size> volume_gains{};

    /// The zero-order-hold mask the part-volume ramp runs on — zero, so one update a sample.
    unsigned volume_ramp_mask = 0;

    /// Where the block loop sits within a control tick, for the per-part send slew.
    int block_tick = 0;

    /// The wet levels' coefficient smoothers, and the per-sample gains they produce.
    MatrixRamp reverb_level_ramp;
    MatrixRamp chorus_level_ramp;
    MatrixRamp delay_level_ramp;
    MatrixRamp chorus_to_reverb_ramp;
    MatrixRamp chorus_to_delay_ramp;
    MatrixRamp delay_to_reverb_ramp;
    std::array<double, block_size> level_gains{};

    std::array<float, block_size> reverb_bus{};
    std::array<float, block_size> chorus_bus{};
    std::array<float, block_size> delay_bus{};
    std::array<float, block_size> wet_left{};
    std::array<float, block_size> wet_right{};

    // The three cross-feeds between the networks. They are separate buffers rather than additions
    // to the send buses because a network runs *after* its bus has been filled and mixed: writing
    // the chorus's output back into `reverb_bus` would be a block late, where the module's is not.
    //
    // `*_mono` is what each network produces -- its tap sum, ahead of its return level -- and the
    // scaled feeds are what the ramped sends make of it.
    std::array<float, block_size> chorus_mono{};
    std::array<float, block_size> delay_mono{};
    std::array<float, block_size> chorus_to_reverb{};
    std::array<float, block_size> chorus_to_delay{};
    std::array<float, block_size> delay_to_reverb{};
    std::array<double, block_size> send_gains{};
    std::array<float, block_size> block_left{};
    std::array<float, block_size> block_right{};

    /// The dry path of the parts that switched the EQ on, kept apart until it has been filtered.
    ///
    /// The engine expresses this as a bus number rather than a buffer: a voice's dry destination is
    /// bus `0x33` when its part has the EQ on and `0x3a` when it does not. The sends are untouched
    /// either way -- only the dry path detours.
    std::array<float, block_size> eq_left{};
    std::array<float, block_size> eq_right{};

    /// The dry path of the parts routed into the insertion EFX (`40 4x 22`), and the block's
    /// output. The engine expresses the input as bus `0x3e` with both send buses forced to the
    /// null bus; the block's own common send levels replace the per-part sends.
    std::array<float, block_size> efx_in_left{};
    std::array<float, block_size> efx_in_right{};
    std::array<float, block_size> efx_out_left{};
    std::array<float, block_size> efx_out_right{};

    // How much of the current block has been handed out. Blocks are always rendered whole, whatever
    // the caller asks for, because a voice's control tick is counted in them.
    int block_offset = block_size;

    /// Internal chunks rendered since the engine started, which is the clock the event pipeline
    /// runs on.
    ///
    /// Not host calls: `render` serves whatever is left in the block buffer before rendering
    /// anything new, so a host asking for 100 samples leaves 28 of a chunk behind and the next call
    /// starts part-way through the grid. The chunk boundaries drift against the host's, and a
    /// deferral counted in host calls would drift with them.
    std::int64_t block_index = 0;

    /// The module's input queue is finite, and it drops what will not fit **without saying so**.
    ///
    /// `TG_flushMidi` moves each ring entry into the ready buffer only `if (sVar4 < DAT_181a63492)`
    /// and discards it otherwise -- no error, no back-pressure. That buffer is `0x181a634a0` to
    /// `0x181a654a0`, 8192 bytes of four-byte USB-MIDI packets, and its bound is initialised to
    /// **2048** by `_DAT_181a63490 = 0x8000000` (count in the low half, capacity in the high). Read
    /// back out of the running engine, it is still 2048.
    ///
    /// A channel message is one packet; a SysEx of `n` bytes is `ceil(n / 3)`, since each packet
    /// carries at most three data bytes. Counted that way against `darkness3.mid`'s opening tick,
    /// the cumulative total passes 2048 at the 47th message -- 2045 before it, 2091 after -- which
    /// is exactly where a probe watching a trailing program change stops seeing it applied.
    ///
    /// **This is not robustness, it is fidelity.** That file opens with 104 events on one tick:
    /// thirty `48` patch-dump messages, thirty `49` drum-set messages, then nine program changes,
    /// every one of them past the limit. The module never receives them, so it plays the patches
    /// the dump chose. An engine that accepts everything plays the file's instead -- a piano where
    /// the module plays a string ensemble -- and implementing the bulk dump correctly is what made
    /// that audible, because before it both engines ignored the dump and agreed by accident.
    static constexpr int flush_capacity_packets = 2048;

    /// 10 ms at 32 kHz: the module's control tick, and the block the oracle harness drives it in.
    static constexpr int control_tick_samples = 320;

    std::int64_t flush_block = -1;
    int flush_packets = 0;

    /// Set while a caller rebuilds state through `SequencePlayer::replay_to`. The queue's bound
    /// models what the module drops from a *stream*; a reconstruction is not one.
    bool replaying_state = false;

    /// Whether an event of `packets` fits in the flush that will carry it.
    ///
    /// Keyed on the control tick the event's own offset falls in, not on the block being
    /// rendered. A caller hands over a whole stretch at once -- `SequencePlayer::dispatch_within`
    /// walks every event up to the end of the buffer before a single sample is produced -- so
    /// `block_index` has not moved yet and would put the entire song in one window.
    ///
    /// The window is a control tick because that is what the bound is actually measuring.
    /// `DAT_181a63490` is the ready buffer's *occupancy*, not a per-message counter, and
    /// `TG_flushMidi` never clears it: `midi_drain_ready_to_ports` empties the buffer outright
    /// (`while (DAT_181a63490 != 0)`) on the way through `TG_Process`. So what has to fit is
    /// whatever a host hands over between two of those drains. `TG_flushMidi` is exported and
    /// called by the host, and the harness that renders every oracle fixture feeds a 320-sample
    /// block, flushes, and processes -- 10 ms at 32 kHz, the module's own 100 Hz control tick.
    ///
    /// Charges `packets` whether or not it fits, so a message that overflows does not leave room
    /// for a later, smaller one: the ring is walked in arrival order and the drop is positional.
    ///
    /// `starts_window` is `ToneGeneratorOptions::flush_before_sysex` reaching this far: a host
    /// flushing ahead of a message opens a window for it. On the module that buys nothing, because
    /// the bound is on the buffer only `TG_Process` empties -- measured, and the option's own
    /// documentation carries the reading -- so this is a deliberate departure and not a model of
    /// anything.
    [[nodiscard]] bool accepts(int sample_offset, int packets, bool starts_window = false) noexcept
    {
        if (replaying_state) {
            return true;
        }
        const std::int64_t slot =
            (block_index * ToneGenerator::block_size + sample_offset) / control_tick_samples;
        if (flush_block != slot || starts_window) {
            flush_block = slot;
            flush_packets = 0;
        }
        const bool room = flush_packets < flush_capacity_packets;
        flush_packets += packets;
        return room;
    }

    /// The module's output stage, run over each chunk on its way to the host.
    OutputFilter output_filter;

    /// A channel message waiting for its chunk, exactly as the module holds one.
    ///
    /// The port and the raw status are kept rather than the resolved part, because the engine
    /// matches parts to a message when it *dispatches* it: a SysEx that repoints a part's receive
    /// channel in between is meant to take effect first.
    struct PendingChannel {
        std::int64_t due = 0;
        int port = 0;
        int status = 0;
        int data1 = 0;
        int data2 = 0;
    };

    std::vector<PendingChannel> pending_channel;

    /// A SysEx message waiting for its chunk. The bytes are copied because the caller's span is
    /// only good for the call.
    struct PendingSysex {
        std::int64_t due = 0;
        int port = 0;
        std::vector<std::uint8_t> bytes;
    };

    std::vector<PendingSysex> pending_sysex;

    /// Applies a SysEx message.
    void dispatch_sysex(int port, std::span<const std::uint8_t> bytes);

    /// A sample offset in the host's rate, as the milliseconds the module stamps a message with.
    ///
    /// `offset * 1000 / host_sample_rate`, truncated -- and a millisecond is one 32-sample chunk at
    /// the engine's rate, which is why the module can compare the stamp against a chunk count.
    [[nodiscard]] int stamp_milliseconds(int sample_offset) const noexcept
    {
        if (sample_offset <= 0 || options.host_sample_rate <= 0) {
            return 0;
        }
        return static_cast<int>((static_cast<std::int64_t>(sample_offset) * 1000)
                                / options.host_sample_rate);
    }

    /// Applies a channel message to whichever parts are listening.
    void dispatch_channel(int port, int status, int data1, int data2);

    /// Runs everything the pipeline has held until this chunk.
    void drain_events();

    std::optional<Reverb> reverb;
    std::optional<Chorus> chorus;
    /// Where the chorus LFO starts, for matching a reference render. See
    /// `ToneGenerator::seed_chorus_phase`.
    int chorus_phase_seed = 0;
    std::optional<SystemDelay> delay;

    /// The insertion EFX block, built the first time a stream touches it — a file that never
    /// sends `40 03` or `40 4x 22` pays nothing for its existence.
    std::optional<InsertionEffect> efx;

    /// The latched `40 03 00` type MSB; the LSB write commits the pair.
    int efx_type_msb = 0;

    void ensure_efx()
    {
        if (!efx && options.efx) {
            efx.emplace(notes->rom());
        }
    }

    /// The four-band EQ, which is one block for the whole module rather than one per part -- parts
    /// opt into it, they do not each get their own.
    Equalizer equalizer{EffectPresets::defaults()};

    std::optional<int> reverb_type;
    std::optional<int> chorus_type;
    std::optional<int> delay_type;

    /// The live reverb and chorus parameter rows.
    ///
    /// A macro loads a row; `40 01 31`-`37` and `40 01 39`-`40` overwrite bytes in it. Keeping the
    /// row rather than only the macro number is what lets a single-parameter edit mean anything --
    /// there is no preset underneath for it to sit on top of, the row *is* the parameters.
    std::array<std::uint8_t, EffectProgrammer::reverb_row_bytes> reverb_row{};
    std::array<std::uint8_t, EffectProgrammer::chorus_row_bytes> chorus_row{};
    bool reverb_row_loaded = false;
    bool chorus_row_loaded = false;
    bool reverb_row_edited = false;
    bool chorus_row_edited = false;

    /// The live delay parameter row, `40 01 51`-`5A`.
    ///
    /// Ten values in exactly the order the stored preset table holds them, which is why the address
    /// maps to an index by subtraction and nothing has to be reordered: pre-LPF, time, ratio left
    /// and right, level centre / left / right, level, feedback, send to reverb.
    ///
    /// Two of those ten never reach `SystemDelay` -- `[7]` and `[9]` are the wet level and the send
    /// into the reverb, which are ramped in the mixer. They are kept in the row anyway because the
    /// row *is* the delay block as the stream sees it, and a macro reload has to restore all ten.
    std::array<int, EffectProgrammer::delay_preset_stride> delay_row{};
    bool delay_row_loaded = false;
    bool delay_row_edited = false;
    bool delay_built_from_row = false;

    /// Wet levels, `40 01 33`, `40 01 3A` and `40 01 58`, which are not coefficients.
    ///
    /// They scale what leaves each network rather than shaping it, so they are applied at the mix
    /// and not folded into a preset. 0x40 is the power-on value and means unity.
    ///
    /// The delay's is the newest of the three and used to be neither: it was compiled into the tap
    /// gains, so it could not ramp and `40 01 58` did nothing at all. All ten stored presets carry
    /// 64, which is why only a stream that sends that address -- `roland_sc88_y05.mid` sends 15 --
    /// could tell.
    int reverb_level = 0x40;
    int chorus_level = 0x40;
    int delay_level = 0x40;

    /// The three routes between the networks, `40 01 3F`, `40 01 40` and `40 01 5A`.
    ///
    /// Coefficients of the same matrix and ramped the same way, but quantised over 128 rather than
    /// 64 -- see `MatrixRamp::send_shift`. Every stored macro and nine of the ten delay presets
    /// leave all three at zero, so on most streams these ramps hold at silence and cost a
    /// multiply-by-nothing; the tenth delay preset carries 36, and files do send the addresses.
    int chorus_to_reverb_level = 0;
    int chorus_to_delay_level = 0;
    int delay_to_reverb_level = 0;

    /// The level that means unity, which is also the power-on value.
    static constexpr int unity_level = 0x40;

    /// Loads the delay's row from the macro's stored preset, the reverb and chorus equivalent.
    void load_delay_row()
    {
        const auto& stored =
            EffectPresets::defaults()
                .delay()
                .raw_presets[static_cast<std::size_t>(delay_type.value_or(0))];
        std::copy(stored.begin(), stored.end(), delay_row.begin());
        delay_level = delay_row[7];
        delay_to_reverb_level = delay_row[9];
        delay_row_loaded = true;

        // A macro replaces the row, so a network standing on an *edited* one has to be rebuilt --
        // including when the type has not changed, which comparing types alone would miss. When it
        // was not edited, nothing is marked, and reselecting a type keeps the tail it already has.
        delay_row_edited = delay_built_from_row;
    }

    /// Loads a macro's row, which is what selecting a type does before any edit.
    /// Fills the reverb row from the current macro, the way a macro select does.
    ///
    /// Split out from `load_macro_rows` so a *reverb* edit arriving before any macro select can
    /// seed its own row without also reloading the chorus's and discarding whatever a stream has
    /// already put there. A macro select still does both, because that is what the module does.
    void load_reverb_row()
    {
        reverb_row = EffectProgrammer::reverb_macro_row(notes->rom(), reverb_type.value_or(4));
        reverb_level = reverb_row[2];
        reverb_row_loaded = true;
        reverb_row_edited = false;
    }

    void load_chorus_row()
    {
        chorus_row = EffectProgrammer::chorus_macro_row(notes->rom(), chorus_type.value_or(2));
        chorus_level = chorus_row[1];
        chorus_to_reverb_level = chorus_row[6];
        chorus_to_delay_level = chorus_row[7];
        chorus_row_loaded = true;
        chorus_row_edited = false;
    }

    void load_macro_rows()
    {
        load_reverb_row();
        load_chorus_row();
    }

    // What each effect was last built for. Distinct from the selection above so that reselecting
    // the type a network already has does not throw its tail away.
    std::optional<int> reverb_built_for = -1;
    std::optional<int> chorus_built_for = -1;
    std::optional<int> delay_built_for = -1;

    double output_gain = 1.0;
    std::optional<int> drum_map_row;
    // One per (port, map), indexed `port * 2 + map`. The module keeps eight drum buffers and a
    // rhythm part addresses `port * 2 + map` of them (`part_assign_tone` computes exactly that
    // index before the kit record is copied in), so two rhythm parts on one port hold different
    // kits only when they sit on different maps -- transcendental.mid, whose channel 10 is a
    // MAP2 rhythm part with its own kit beside channel 9's on MAP1. Parts sharing a map share
    // the kit, last program change wins, which is equally the module's behaviour.
    std::vector<int> drum_kit;
    std::int64_t position = 0;
    int note_count = 0;

    // The system-common block, from GS SysEx 40 00 xx.
    //
    // Master tune is kept raw: 0.1-cent units with 0x400 centred, clamped to 0x18-0x7e8 exactly as
    // `sysex_master_tune` does -- which makes one raw step one milli-semitone. Master key shift is
    // 0x40 centred and clamped to 0x28-0x58 (`sysex_master_key_shift`). Master pan is latched but
    // not yet consumed: the mixer has no post-pan master stage to apply it in.
    int master_tune = 0x400;
    int master_key_shift = 0x40;
    int master_pan = 0x40;

    /// The global pitch offset every voice shares, in milli-semitones.
    [[nodiscard]] double master_tune_milli_semitones() const noexcept
    {
        return static_cast<double>(master_tune - 0x400) + ((master_key_shift - 0x40) * 1000.0);
    }

    [[nodiscard]] int effective_drum_map_row() const noexcept
    {
        if (drum_map_row) {
            return *drum_map_row;
        }
        return DrumKitTable::row_for_map(options.map).value_or(0);
    }

    /// Moves a voice out of its slot so it can fade rather than being cut dead.
    void steal(int index);
    void release_slot(int index);
    void recycle(int index);

    // Parts are addressed the way the module addresses them: port times sixteen, plus channel.
    [[nodiscard]] int part_of(int port, int channel) const noexcept
    {
        return ((port & (port_count - 1)) * Sequence::channel_count) + channel;
    }

    // Every port has its own drum part, on the same channel within that port -- unless SysEx
    // `40 1x 15` (use-for-rhythm) has overridden the routing for the part.
    [[nodiscard]] bool is_drum_part(int part) const noexcept
    {
        const Part& state = parts[static_cast<std::size_t>(part)];
        if (state.rhythm >= 0) {
            return state.rhythm > 0;
        }
        return part % Sequence::channel_count == options.drum_channel;
    }

    // Which drum map a rhythm part sits on: use-for-rhythm value 2 is MAP2, 1 or the channel
    // default is MAP1.
    [[nodiscard]] static int map_of(const Part& part) noexcept
    {
        return part.rhythm == 2 ? 1 : 0;
    }

    // Which kit a rhythm part reads, and which its program change writes.
    //
    // GS keeps the module's arrangement: a kit per (port, map), so the port's rhythm part and its
    // MAP2 counterpart have one each. That is a real limit of the module -- it gives the port's
    // drum channel one slot and *shares a second between every other drum part on that port* -- and
    // GS files are rendered against it, so it stays.
    //
    // XG gets a kit per part. It is the mode that makes the limit reachable: bank MSB 127 turns any
    // channel into a drum part, so a file can have five of them, and under the module's arrangement
    // four would fight over one slot and all sound as whichever moved last. Diverging here is
    // deliberate and confined to the mode that needs it -- a GS render cannot reach this branch.
    [[nodiscard]] std::size_t kit_slot(int part) const noexcept
    {
        if (xg_mode) {
            return static_cast<std::size_t>(part);
        }
        return static_cast<std::size_t>((part / Sequence::channel_count) * 2
                                        + map_of(parts[static_cast<std::size_t>(part)]));
    }

    // Which vintage's tone map the part resolves against: bank select LSB 1-3 names one, **4 and
    // anything above lands on SC-8820**, and 0 keeps the configured default.
    //
    // What the module does with an out-of-range LSB is measured, and it is not what this engine
    // did. Sweeping the LSB on a rhythm part -- program 0, one snare, everything else fixed -- the
    // renders fall into four groups, and the top one is open-ended:
    //
    //     LSB 0, 1        identical (0 leaves the part's map alone; the harness had set map 1)
    //     LSB 2           its own kit
    //     LSB 3           its own kit
    //     LSB 4 and up    identical for 4, 5, 6, 8, 10, 42, 64 and 127
    //
    // And it overrides an explicit request rather than being ignored: with the harness setting each
    // part's map by `40 4x 01` first, LSB 42 renders **the same at maps 1, 2, 3 and 4**, byte for
    // byte, and that render is the map-4 one.
    //
    // **Whether that is a clamp to the top of the range or a fall back to the module's own default
    // cannot be told apart here, and probably not at all.** SC-8820 is both, and the DLL offers no
    // way to move the default somewhere else and watch which one follows. The two readings predict
    // the same audio for every input, so this implements the observable behaviour and takes no
    // position on the mechanism. What matters for a caller is the part it fixes: the result does
    // **not** depend on `ToneGeneratorOptions::map`, so asking for an SC-55 render does not keep a
    // part whose file sent LSB 42 on the SC-55 map.
    //
    // Reading it as "not a map, so keep the default" is what made `canyon.mid` render 4.8 dB hot at
    // 125 Hz and 5.0 dB at 8 kHz with a peak ratio of 0.865: it sends bank LSB **42** on its drum
    // channel, so the module plays the SC-8820 kit -- a different snare and a different maracas --
    // where this engine stayed on whichever map the caller asked for. With this, those read
    // +1.3 dB, -0.1 dB and 0.995.
    //
    // XG overrides both. System On puts every part on the XG map, and there is no per-part escape
    // from it while the mode holds -- the bank LSB has become the variation index and no longer
    // names a map at all.
    [[nodiscard]] static int map_index_for(const Part& part) noexcept
    {
        return part.bank_lsb >= 1 ? std::min(part.bank_lsb, 4) : 0;
    }

    [[nodiscard]] ToneMap tone_map_for(const Part& part) const noexcept
    {
        if (xg_mode) {
            return ToneMap::xg;
        }
        if (const int index = map_index_for(part); index != 0) {
            return static_cast<ToneMap>(index);
        }
        return options.map;
    }

    // The drum map row a program change on a drum part resolves against.
    [[nodiscard]] int drum_row_for(const Part& part) const noexcept
    {
        if (xg_mode) {
            const std::optional<int> row = DrumKitTable::row_for_map(ToneMap::xg);
            if (row) {
                return *row;
            }
        }
        if (const int index = map_index_for(part); index != 0) {
            const std::optional<int> row = DrumKitTable::row_for_map(static_cast<ToneMap>(index));
            if (row) {
                return *row;
            }
        }
        return effective_drum_map_row();
    }

    // The bank the melodic lookup is given for a part.
    //
    // GS puts the variation in `bank` and that is the whole story. XG puts it in the LSB, which
    // lands in the same field -- except for bank MSB 64, the SFX voice bank, which is a *column* of
    // the XG map rather than a variation of one. The module reaches it by substituting 0x7D for the
    // bank at resolution time, which is bank LSB 125: the column where program 90 is Submarine
    // rather than the Polysynth it is at LSB 0.
    [[nodiscard]] int lookup_bank_for(const Part& part) const noexcept
    {
        if (xg_mode && part.xg_bank_msb == 0x40) {
            return 0x7D;
        }
        return part.bank;
    }

    // Whether an XG address names a part this engine actually has.
    //
    // XG addresses up to sixty-four parts and this engine can be configured for sixteen, thirty-two
    // or all sixty-four. A part above the count is *ignored*: the module indexes its part array
    // without a bounds check and writes off the end of it, and the alternative of wrapping would
    // quietly apply one part's settings to another.
    [[nodiscard]] bool xg_part_in_range(int part) const noexcept
    {
        return part >= 0 && part < static_cast<int>(parts.size());
    }

    void apply_channel(int part_index, Part& part, int status, int data1, int data2);
    void program_change(int part_index, Part& part, int program);
    void control_change(int channel, Part& part, int controller, int value);
    void commit_data_entry_msb(int channel, Part& part, int value);
    void commit_data_entry_lsb(Part& part, int value);
    void
    gs_part_parameter(int part_index, Part& part, int address, std::span<const std::uint8_t> data);

    void gs_bulk_dump(int block_port, int address, std::span<const std::uint8_t> data);

    void gs_drum_bulk_dump(int block_port, int a2, std::span<const std::uint8_t> data);

    /// The patch bulk dump's walk, carried across messages.
    ///
    /// `48 <a2> <a3>` is not a page, it is a position in a 16-bit address space -- the engine's
    /// `g_sysex_addr_idx`, built as `(a2 << 8) + a3` -- and the dump is **one continuous walk** over
    /// all sixteen parts rather than a set of independent messages. Most addresses carry no part
    /// and no destination of their own: they simply continue from wherever the last message left
    /// off. So a port has to hold what the module holds in `g_cur_part_base` and
    /// `g_sysex_write_cursor`, or it cannot decode the second message at all.
    ///
    /// `-1` in either means no anchor has been established, and a continuation arriving in that
    /// state is dropped. **The module does not do that** -- it walks from whatever those globals
    /// happen to hold and reads or writes off the end of the part array. That is a real fault,
    /// reachable from a file, and it is the one thing here deliberately not reproduced.
    int bulk_part_block = -1;
    int bulk_record_offset = -1;

    /// The program a dump has supplied but not yet acted on.
    ///
    /// Tone selection is three record bytes and the order is the point. `+0x3d4` is the bank MSB,
    /// `+0x3d5` the program, and `+0x3d9` both the melodic/drum selector *and* the moment the
    /// change fires -- `part_param_write_all` writes that byte and then branches on its own bits
    /// 0x10 and 0x20 into `part_program_change` or `drum_part_program_change`. Applying the program
    /// when it arrives instead would resolve the patch before the byte that says whether the part
    /// is a drum part at all, and would be overwritten by everything the record still has to say.
    int bulk_pending_program = -1;

    /// Whether XG System On has been seen and not since revoked.
    ///
    /// XG is a whole second parameter dialect rather than a set of extra messages, and the module
    /// swaps its SysEx parser wholesale when this changes. It leaves on any Roland message and on
    /// the GM resets, which means a file that mixes dialects flips the instrument between them
    /// rather than layering them.
    bool xg_mode = false;

    /// The type MSB of each effect, held until its LSB arrives.
    ///
    /// XG sends the pair as two addresses and the translation needs both, so the MSB is buffered
    /// the way the module buffers it.
    int xg_reverb_type_msb = 0;
    int xg_chorus_type_msb = 0;

    void xg_sysex(std::span<const std::uint8_t> bytes);
    void xg_multi_part(int part_index, Part& part, int parameter, int value);
    void xg_program_change(int part_index, Part& part, int program);
    void xg_resolve_program(int part_index, Part& part, int program);
    void xg_effect1(int parameter, std::span<const std::uint8_t> data);
    void leave_xg_mode();
    void gs_drum_setup(int port, int map, int parameter, int key,
                       std::span<const std::uint8_t> data);
    void release_sustained(int channel, Part& part);
    void flush_part_voices(int channel);

    // Returns every part to power-on state without touching the clocks: what a GM System On, GS
    // reset or system-mode-set does mid-stream.
    /// Lets a note's bit-5 LFO nodes inherit this part's standing ones. True if any did.
    bool adopt_standing_nodes(PendingNote& note);

    /// Installs a note's bit-5 LFO nodes as this part's standing ones.
    void take_standing_nodes(PendingNote& note);

    void stream_reset();
    [[nodiscard]] bool any_voice_on(int channel) const;
    void stop_note(int channel, int note, int damper = 0);
    void start_note(int channel, int note, int velocity);
    void start_drum(int channel, int note, int velocity);
    /// Seeds one part's LFO nodes for a chunk: the first note draws, the rest copy.
    void seed_part_lfo_nodes(std::span<PendingNote> batch);
    /// Reads one already-seeded note's parameters and installs its voices.
    void read_pending_note(PendingNote& note);
    /// Takes a slot for a voice, stealing if the pool is full. Dispatch-time, arrival order.
    [[nodiscard]] int allocate_slot(int channel, int note, int velocity, int group);
    /// Puts a fully-read voice into the slot `allocate_slot` returned. Block-ordered pass.
    void install(int slot, VoiceSetup&& setup);

    struct Envelopes {
        SegmentEnvelope amplitude;
        SegmentEnvelope cutoff;
        int cutoff_base = 0;
    };

    [[nodiscard]] Envelopes envelopes(int tone_number,
                                      const PartialParameters& partial,
                                      int key,
                                      int velocity,
                                      const PartModifiers& modifiers,
                                      std::optional<int> rate_key = std::nullopt);

    void render_block();
    void mix_voice(PartialVoice& voice, std::span<float> left, std::span<float> right);
    void mix_effects(std::span<float> left, std::span<float> right);
    void ensure_effects();
};

// ---------------------------------------------------------------------------------------------
// Lifetime and accessors
// ---------------------------------------------------------------------------------------------

ToneGenerator::ToneGenerator(NoteRenderer& notes, const ToneGeneratorOptions& options)
    : impl_(std::make_unique<Impl>(notes, options))
{
}

ToneGenerator::ToneGenerator(ToneGenerator&&) noexcept = default;
ToneGenerator& ToneGenerator::operator=(ToneGenerator&&) noexcept = default;
ToneGenerator::~ToneGenerator() = default;

double ToneGenerator::output_gain() const noexcept
{
    return impl_->output_gain;
}

void ToneGenerator::set_output_gain(double gain) noexcept
{
    impl_->output_gain = gain;
}

int ToneGenerator::ports() const noexcept
{
    return impl_->port_count;
}

int ToneGenerator::parts() const noexcept
{
    return impl_->port_count * Sequence::channel_count;
}

std::int64_t ToneGenerator::position() const noexcept
{
    return impl_->position;
}

int ToneGenerator::note_count() const noexcept
{
    return impl_->note_count;
}

int ToneGenerator::active_voices() const noexcept
{
    auto count = static_cast<int>(impl_->dying.size());
    for (const auto& slot : impl_->slots) {
        if (slot) {
            ++count;
        }
    }
    return count;
}

const DrumKeyOverrides& ToneGenerator::drum_setup(int part) const noexcept
{
    return impl_->drum_setup[impl_->kit_slot(part)];
}

const DrumKeyOverrides& ToneGenerator::drum_setup_slot(int slot) const noexcept
{
    return impl_->drum_setup[static_cast<std::size_t>(slot)];
}

const Part& ToneGenerator::part(int index) const noexcept
{
    // Clamped rather than trusted. This has to return a reference and cannot throw, so the choice
    // is between defined data and reading whatever follows the array -- and the caller that gets
    // this wrong is a UI walking `ChannelMask::channel_count` parts on an engine that has fewer,
    // which is a mistake that shows up as plausible garbage rather than as a crash.
    const int last = static_cast<int>(impl_->parts.size()) - 1;
    return impl_->parts[static_cast<std::size_t>(std::clamp(index, 0, last))];
}

const VoicePool& ToneGenerator::voices() const noexcept
{
    return impl_->pool;
}

bool ToneGenerator::polyphony_limit_reached() const noexcept
{
    return impl_->pool.limit_was_reached();
}

int ToneGenerator::stolen_voices() const noexcept
{
    return impl_->pool.steal_count();
}

int ToneGenerator::voice_slots() const noexcept
{
    return impl_->pool.high_water();
}

int ToneGenerator::drum_kit() const noexcept
{
    return drum_kit_for(0);
}

int ToneGenerator::drum_kit_for(int port) const noexcept
{
    // What the port's default rhythm part carries, which is what a kit display means by "the kit".
    // Asked for by part rather than by slot number, because the slot layout is not the same in both
    // modes -- under XG a kit belongs to a part, and slot 0 is part 0's rather than the drum
    // channel's.
    const int base = (port & (impl_->port_count - 1)) * Sequence::channel_count;
    return impl_->drum_kit[impl_->kit_slot(base + impl_->options.drum_channel)];
}

std::optional<int> ToneGenerator::drum_map_row() const noexcept
{
    return impl_->drum_map_row;
}

void ToneGenerator::set_drum_map_row(std::optional<int> row) noexcept
{
    impl_->drum_map_row = row;
}

int ToneGenerator::effective_drum_map_row() const noexcept
{
    return impl_->effective_drum_map_row();
}

bool ToneGenerator::xg_mode() const noexcept
{
    return impl_->xg_mode;
}

void ToneGenerator::seed_chorus_phase(int phase) noexcept
{
    // Remembered as well as applied, because the chorus is rebuilt whenever a file selects a macro
    // and a seed applied only to the object standing now would not survive the first one.
    impl_->chorus_phase_seed = phase;
    if (impl_->chorus) {
        impl_->chorus->set_phase(phase);
    }
}

ToneMap ToneGenerator::part_tone_map(int index) const noexcept
{
    return impl_->tone_map_for(part(index));
}

int ToneGenerator::part_lookup_bank(int index) const noexcept
{
    return impl_->lookup_bank_for(part(index));
}

bool ToneGenerator::part_is_drum(int index) const noexcept
{
    const int clamped = std::clamp(index, 0, static_cast<int>(impl_->parts.size()) - 1);
    return impl_->is_drum_part(clamped);
}

int ToneGenerator::part_rx_channel(int index) const noexcept
{
    const int clamped = std::clamp(index, 0, static_cast<int>(impl_->parts.size()) - 1);
    return impl_->parts[static_cast<std::size_t>(clamped)].rx_channel;
}

int ToneGenerator::part_drum_kit(int index) const noexcept
{
    if (!part_is_drum(index)) {
        return -1;
    }
    const int clamped = std::clamp(index, 0, static_cast<int>(impl_->parts.size()) - 1);
    return impl_->drum_kit[impl_->kit_slot(clamped)];
}

void ToneGenerator::reset()
{
    for (int i = 0; i < static_cast<int>(impl_->slots.size()); ++i) {
        impl_->recycle(i);
    }

    for (auto& voice : impl_->dying) {
        voice->kill();
        impl_->spare.push_back(std::move(voice));
    }
    impl_->dying.clear();
    impl_->pool.reset();

    for (int i = 0; i < impl_->port_count * Sequence::channel_count; ++i) {
        Part& part = impl_->parts[static_cast<std::size_t>(i)];
        part.reset();
        part.rx_channel = i % Sequence::channel_count;
    }
    // A reset returns the parts to power-on, and the module's descriptor slots go with them.
    for (Impl::StandingLfoNodes& nodes : impl_->standing_lfo) {
        nodes.clear();
    }
    impl_->master_tune = 0x400;
    impl_->master_key_shift = 0x40;
    impl_->master_pan = 0x40;
    // Back to the configured starting mode, not to GS: `reset` undoes what the music did, and the
    // host's choice of map is not something the music did.
    impl_->xg_mode = impl_->options.map == ToneMap::xg;

    if (impl_->reverb) {
        impl_->reverb->reset();
    }
    if (impl_->chorus) {
        impl_->chorus->reset();
    }
    if (impl_->delay) {
        impl_->delay->reset();
    }

    // The generator is reseeded *here* and nowhere else, because power-on is the only place the
    // module reseeds it: `engine_init_tasks_ports @180084c60` writes 0xEFA6/0x9C23, and it is
    // reached only from `TG_initialize @1800888a0`. A GS Reset in the stream does not -- measured,
    // not inferred, with `scdec resetstate`: the LFSR reads 0xE9E7/0xF13C going into a reset and
    // 0xBA79/0xC4F1 after it, never returning to the seed. That is why the same note renders
    // differently the second time through one instance, at about -51 dB, and why a batch of notes
    // through one instance cannot reproduce one-note-per-instance renders.
    //
    // Without this the reset was neither of the module's two behaviours: it dropped the effect
    // tails, which only power-on does, while keeping the draw position, which only an in-stream
    // reset does. `stream_reset` is the other one and correctly leaves both alone.
    impl_->notes->noise().reset();

    std::fill(impl_->drum_kit.begin(), impl_->drum_kit.end(), 0);
    for (DrumKeyOverrides& planes : impl_->drum_setup) {
        planes.reset();
    }
    impl_->position = 0;
    impl_->block_offset = block_size;
    impl_->note_count = 0;
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

void ToneGenerator::send(const MidiEvent& message)
{
    send(0, message);
}

void ToneGenerator::send(int port, const MidiEvent& message)
{
    send_at(immediately, port, message);
}

void ToneGenerator::send_at(int sample_offset, int port, const MidiEvent& message)
{
    if (message.kind == MidiEventKind::sysex) {
        if (!message.sysex.empty()) {
            send_sysex_at(sample_offset, port, message.sysex);
        }
        return;
    }
    send_channel_at(sample_offset, port, message.status, message.data1, message.data2);
}

void ToneGenerator::send_channel(int status, int data1, int data2)
{
    send_channel_at(immediately, 0, status, data1, data2);
}

void ToneGenerator::send_channel_at(int sample_offset, int status, int data1, int data2)
{
    send_channel_at(sample_offset, 0, status, data1, data2);
}

void ToneGenerator::send_channel(int port, int status, int data1, int data2)
{
    send_channel_at(immediately, port, status, data1, data2);
}

void ToneGenerator::send_channel_at(int sample_offset, int port, int status, int data1, int data2)
{
    // Held for the pipeline when one is configured, applied on arrival when it is not. The module
    // always holds: `TG_ShortMidiIn` only puts the message in a ring, and nothing reaches a part
    // until `TG_Process` walks it out. See `ToneGeneratorOptions::event_delay_blocks`.
    if (!impl_->accepts(sample_offset, 1)) {
        return;
    }

    const int stamp = impl_->stamp_milliseconds(sample_offset);
    if (impl_->options.event_delay_blocks > 0 || stamp > 0) {
        // Two separate waits, because the module has two. The stamp decides which chunk the message
        // is *released* on -- `TG_Process` walks the ring only as far as its counter has reached --
        // and the staging count is how long it then takes to reach a part. Due one past the sum,
        // because the drain runs after the chunk counter has already advanced.
        impl_->pending_channel.push_back(
            Impl::PendingChannel{impl_->block_index + stamp
                                     + impl_->options.event_delay_blocks + 1,
                                 port,
                                 status,
                                 data1,
                                 data2});
        return;
    }
    impl_->dispatch_channel(port, status, data1, data2);

    // Undelayed messages are applied on arrival rather than waiting for a chunk, so their notes
    // start on arrival too. Holding them for the next `render_block` would delay every note landing
    // on a chunk boundary by a chunk. `event_delay_blocks` at zero means there is no chunk for the
    // messages to share, so there is nothing to order them within.
    impl_->start_pending_voices();
}

void ToneGenerator::Impl::dispatch_channel(int port, int status, int data1, int data2)
{
    // Parts are matched by their receive channel rather than indexed by it, the way the engine
    // walks a per-channel list of listening parts: SysEx can point several parts at one channel,
    // or detach a part entirely.
    const int incoming = status & 0x0F;
    const int base = (port & (port_count - 1)) * Sequence::channel_count;
    for (int i = 0; i < Sequence::channel_count; ++i) {
        const int index = base + i;
        Part& part = parts[static_cast<std::size_t>(index)];
        if (part.rx_channel == incoming) {
            apply_channel(index, part, status & 0xF0, data1, data2);
        }
    }
}

void ToneGenerator::Impl::drain_events()
{
    // SysEx before channel messages within a chunk. The module's ring is one queue in arrival
    // order, and this is not that -- but the case it decides is a reset or a mode change arriving
    // alongside notes, where acting on the mode first is what the module does by virtue of the
    // host having sent it first.
    if (!pending_sysex.empty()) {
        std::size_t kept = 0;
        for (std::size_t i = 0; i < pending_sysex.size(); ++i) {
            if (pending_sysex[i].due > block_index) {
                if (kept != i) {
                    pending_sysex[kept] = std::move(pending_sysex[i]);
                }
                ++kept;
                continue;
            }
            dispatch_sysex(pending_sysex[i].port, pending_sysex[i].bytes);
        }
        pending_sysex.resize(kept);
    }

    if (pending_channel.empty()) {
        return;
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pending_channel.size(); ++i) {
        const PendingChannel event = pending_channel[i];
        if (event.due > block_index) {
            pending_channel[kept++] = event;
            continue;
        }
        dispatch_channel(event.port, event.status, event.data1, event.data2);
    }
    pending_channel.resize(kept);
}

void ToneGenerator::Impl::apply_channel(
    int part_index, Part& part, int status, int data1, int data2)
{
    const int channel = part_index;

    switch (status) {
    case 0x90:
        if (data2 > 0) {
            if (!part.rx.notes) {
                break;
            }
            // Re-striking a still-open note takes the old voice first.
            stop_note(channel, data1);

            // The new strike supersedes a note-off the pedal is still holding for this note.
            std::erase(part.sustained, data1);
            std::erase(part.sostenuto_captured, data1);
            std::erase(part.sostenuto_released, data1);

            // A fresh strike does **not** clear the key's poly pressure. See `Part::poly_pressure`:
            // that was the obvious guess and the engine disagrees, measurably.

            start_note(channel, data1, data2);
            break;
        }
        [[fallthrough]];

    case 0x80:
        if (!part.rx.notes) {
            break;
        }
        if (part.sostenuto_down
            && std::find(part.sostenuto_captured.begin(), part.sostenuto_captured.end(), data1)
                   != part.sostenuto_captured.end()) {
            // A captured note's release is deferred until the sostenuto pedal lifts.
            part.sostenuto_released.push_back(data1);
        } else if (part.damper_down()) {
            part.sustained.push_back(data1);
        } else {
            stop_note(channel, data1, part.damper);
        }
        break;

    case 0xA0:
        // Poly pressure reaches the modulation matrix on the module (`poly_aftertouch_apply`),
        // carrying the part's depths and the pressure belonging to this key.
        if (part.rx.poly_pressure && data1 >= 0 && data1 < 128) {
            part.poly_pressure[static_cast<std::size_t>(data1)] =
                static_cast<std::uint8_t>(data2 & 0x7F);
        }
        break;

    case 0xB0:
        control_change(channel, part, data1, data2);
        break;

    case 0xC0:
        if (part.rx.program_change) {
            // In XG the bank MSB decides melodic against drums, so the program change has to go
            // through that decision rather than straight to the melodic lookup.
            if (xg_mode) {
                xg_program_change(part_index, part, data1);
            } else {
                program_change(part_index, part, data1);
            }
        }
        break;

    case 0xD0:
        // Channel pressure is likewise a matrix source (`channel_pressure_apply`); the part's
        // matrix sums it per control tick alongside the other five sources.
        if (part.rx.channel_pressure) {
            part.channel_pressure = data1;
        }
        break;

    case 0xE0:
        if (part.rx.pitch_bend) {
            part.bend = data1 | (data2 << 7);
        }
        break;

    default:
        break;
    }
}

void ToneGenerator::Impl::program_change(int part_index, Part& part, int program)
{
    part.program = program;
    // `part_program_change` reaches `part_voice_desc_reset`, which empties this part's LFO node
    // slots. The next note allocates parentless nodes and draws again, whatever bit 5 says.
    standing_lfo[static_cast<std::size_t>(part_index)].clear();
    // The tone loader fills the per-program tone-modify slots and forces the ninth back to centre
    // on every program change. No shipped tone populates it, so in practice this is a reset: a
    // file that sets `40 4x 38` and then changes program loses it, where `40 1x 38` survives.
    part.envelope_delay_tone = 0x40;
    if (is_drum_part(part_index)) {
        const std::optional<int> kit = notes->drums().kit_for_program(program, drum_row_for(part));
        if (kit) {
            // An undefined program leaves the current kit in place rather than falling back to
            // Standard.
            drum_kit[kit_slot(part_index)] = *kit;
            // **And every per-key override goes with it.** A program change reloads the kit record
            // into the part's per-key planes, so anything the drum-setup NRPNs or SysEx wrote there
            // is overwritten. Measured on the module with `scdec gsdrumnrpn`: write pan 100 to key
            // 49, strike it, send a program change, strike again -- the plane reads the kit's own
            // 84. Level, coarse pitch, reverb and chorus all behave the same, and it happens even
            // when the program selects the kit already loaded.
            //
            // Conditional on the kit resolving, which is why this sits inside the guard rather than
            // beside it. Programs 0, 1 and 8 are Standard 1, Standard 2 and Room, and all three
            // clear; 7 and 63 name no kit, and on those the overrides survive. So the reload is
            // what clears them, not the program change.
            //
            // And only **this part's own buffer**: measured on the module, a setup written to the
            // other kit slot survives every program change untouched. See `drum_setup`.
            drum_setup[kit_slot(part_index)].reset();
        }
    }
}

void ToneGenerator::Impl::leave_xg_mode()
{
    if (!xg_mode) {
        return;
    }
    xg_mode = false;
    stream_reset();
}

// XG bank select, which is not GS bank select with different numbers.
//
// The MSB chooses the *kind* of sound and the LSB the variation within it, the opposite way round
// from GS. Drums are reachable from bank select alone, on any part: MSB 0x7F selects a drum kit by
// program and MSB 0x7E the SFX kits, whose two programs sit at 120 and 121 in the same row, which
// is what the module's `+0x78` program offset is reaching. Under GS that is impossible without the
// use-for-rhythm SysEx.
// Resolves a program on the routing the part already has, without revisiting that routing.
//
// Kept apart from the bank-select decision because XG Part Mode has to be able to set the routing
// and then re-resolve: folding the two together let a program change taken on behalf of Part Mode
// overwrite the very decision that asked for it.
void ToneGenerator::Impl::xg_resolve_program(int part_index, Part& part, int program)
{
    if (!is_drum_part(part_index)) {
        program_change(part_index, part, program);
        return;
    }

    int kit_program = program;
    if (part.xg_bank_msb == 0x7E) {
        kit_program += 0x78;
    } else if (kit_program > 0x77) {
        kit_program = 0;
    }

    part.program = program;
    const std::optional<int> kit =
        notes->drums().kit_for_program(kit_program, drum_row_for(part));
    if (kit) {
        drum_kit[kit_slot(part_index)] = *kit;
        // The kit reload takes the per-key overrides with it, as on the GS path above -- and only
        // the buffer this part reads.
        drum_setup[kit_slot(part_index)].reset();
    }
}

void ToneGenerator::Impl::xg_program_change(int part_index, Part& part, int program)
{
    const int msb = part.xg_bank_msb;

    // A drum bank makes a drum part of any channel. A melodic bank does *not* make a melodic part
    // of one: it returns the part to its default, which for channel 10 is still drums. Bank select
    // cannot take the default drum part away in XG -- only XG Part Mode, or the GS use-for-rhythm
    // SysEx, does that, and both say so explicitly rather than as a side effect of choosing a
    // sound. Reading a melodic bank as "not drums" silences the percussion of any file that sets a
    // bank on channel 10, which is a great many of them.
    //
    // A part with no bank select at all keeps its default untouched, so a file carrying nothing but
    // program changes still has its percussion on channel 10.
    if (msb >= 0) {
        part.rhythm = msb >= 0x7E ? 1 : -1;
    }

    xg_resolve_program(part_index, part, program);
}

// One XG Multi Part parameter that this path can hold and the shared decoder does not cover.
void ToneGenerator::Impl::xg_multi_part(int part_index, Part& part, int parameter, int value)
{
    switch (parameter) {
    // Bank MSB, bank LSB, program. The MSB is remembered and the LSB *is* the lookup bank, which
    // is why it lands in the same field a GS variation does.
    case 0x01:
        part.xg_bank_msb = value;
        return;
    case 0x02:
        part.bank = value;
        return;
    case 0x03:
        xg_program_change(part_index, part, value);
        return;

    // Rcv Channel. 0x7F is "off", which this engine spells as channel 16.
    case 0x04:
        part.rx_channel = value > 0x0F ? Sequence::channel_count : value;
        return;

    // Rcv NRPN, one of the sixteen Rcv switches at `30`-`3f` that `xg_part_rx_switches` maps onto
    // the Rx word at part+0x3d6. Only this one is wired, because only this one is reachable in a
    // state a file can otherwise not get out of: XG System On clears Rx NRPN on every part, so
    // without `08 nn 37` an XG file could never use an NRPN again. The other fifteen default on
    // and stay on, so ignoring them costs nothing until a file turns one off.
    case 0x37:
        part.rx.nrpn = value != 0;
        return;

    // Part Mode: 0 normal, 1 drum, 3/4/5 the numbered drum setups. Anything drum-shaped routes the
    // part to the drum path and re-resolves its program there.
    case 0x07:
        // Part Mode is the explicit routing message, so it sets the routing and re-resolves on it
        // rather than going back through bank select, which would only undo what it just decided.
        part.rhythm = value == 0 ? 0 : 1;
        xg_resolve_program(part_index, part, part.program);
        return;

    // Note Shift, same 0x28-0x58 clamp the GS part key shift uses.
    case 0x08:
        part.key_shift = std::clamp(value, 0x28, 0x58);
        return;

    // Detune, two nibbles high-first. The low nibble arrives as parameter 0x0A and this engine
    // keeps the pair in the GS fine-tune field, which is the same 14-bit quantity.
    case 0x09:
        part.fine_tune = (part.fine_tune & 0x00FF) | ((value & 0x0F) << 8);
        return;
    case 0x0A:
        part.fine_tune = (part.fine_tune & 0x3F00) | ((value & 0x0F) << 4);
        return;

    // Note Limit Low and High.
    case 0x0F:
        part.key_low = value;
        return;
    case 0x10:
        part.key_high = value;
        return;

    default:
        break;
    }

    // Scale Tuning, one entry a pitch class, in the same 0x40-centred units GS uses.
    if (parameter >= 0x41 && parameter <= 0x4C) {
        part.scale_tuning[static_cast<std::size_t>(parameter - 0x41)] = value;
    }
}

// XG Effect1: reverb and chorus type, as an MSB/LSB pair.
//
// The module translates the pair through a small table and *drops* anything it does not find --
// ROOM3, STAGE1/2, PLATE, the CELESTEs and the flangers have no entry at all, and the effect simply
// keeps its previous setting. That silence is reproduced here rather than approximated, because
// guessing a nearest GS macro would put an effect on the part that the module would not have.
void ToneGenerator::Impl::xg_effect1(int parameter, std::span<const std::uint8_t> data)
{
    if (data.empty()) {
        return;
    }

    // `00` is the type MSB with the LSB as its second data byte; `01` is the LSB alone.
    if (parameter == 0x00 || parameter == 0x01) {
        const int msb = parameter == 0x00 ? data[0] : xg_reverb_type_msb;
        const int lsb = parameter == 0x00 ? (data.size() > 1 ? data[1] : 0) : data[0];
        xg_reverb_type_msb = msb;
        if (const std::optional<int> macro = xg_reverb_macro(msb, lsb)) {
            reverb_type = *macro;
        }
        return;
    }

    if (parameter == 0x20 || parameter == 0x21) {
        const int msb = parameter == 0x20 ? data[0] : xg_chorus_type_msb;
        const int lsb = parameter == 0x20 ? (data.size() > 1 ? data[1] : 0) : data[0];
        xg_chorus_type_msb = msb;
        if (const std::optional<int> macro = xg_chorus_macro(msb, lsb)) {
            chorus_type = *macro;
        }
    }
}

void ToneGenerator::Impl::xg_sysex(std::span<const std::uint8_t> bytes)
{
    const XgAddress address = decode_xg_sysex(bytes);

    switch (address.kind) {
    case XgMessage::system_on:
        // Entering is a reset *and* a re-map: every part moves to the XG tone and drum maps, which
        // is why this is not the same as a GM reset with a flag set beside it.
        xg_mode = true;
        stream_reset();
        return;

    case XgMessage::all_parameter_reset:
        stream_reset();
        return;

    case XgMessage::system_parameter:
        if (address.low <= 0x03) {
            // Master tune, four nibbles, same 0x400 centre the GS form uses.
            master_tune = std::clamp(master_tune, 0, 0x7FF);
        } else if (address.low == 0x04) {
            for (Part& part : parts) {
                part.set_master(address.value);
            }
        } else if (address.low == 0x06) {
            master_key_shift = std::clamp(address.value, 0x28, 0x58);
        }
        return;

    case XgMessage::effect1:
        xg_effect1(address.low, bytes.subspan(7, bytes.size() - 8));
        return;

    case XgMessage::multi_part: {
        if (!xg_part_in_range(address.part)) {
            return;
        }
        Part& part = parts[static_cast<std::size_t>(address.part)];
        if (const std::optional<ControlUpdate> update = decode_xg_multi_part(address)) {
            if (apply_control_update(part, *update)) {
                return;
            }
        }
        xg_multi_part(address.part, part, address.low, address.value);
        return;
    }

    // Drum Setup writes per-key overrides, which this engine has machinery for but reaches only
    // through NRPN today. Recognised so it is visibly unhandled rather than silently mistaken for
    // something else.
    case XgMessage::drum_setup:
    case XgMessage::none:
        return;
    }
}

void ToneGenerator::send_sysex(std::span<const std::uint8_t> bytes)
{
    send_sysex_at(immediately, 0, bytes);
}

void ToneGenerator::send_sysex_at(int sample_offset, std::span<const std::uint8_t> bytes)
{
    send_sysex_at(sample_offset, 0, bytes);
}

void ToneGenerator::send_sysex(int port, std::span<const std::uint8_t> bytes)
{
    send_sysex_at(immediately, port, bytes);
}

void ToneGenerator::send_sysex_at(int sample_offset,
                                  int port,
                                  std::span<const std::uint8_t> bytes)
{
    // Three data bytes to a packet, which is how the module's input path carries a long message.
    if (!impl_->accepts(sample_offset, static_cast<int>((bytes.size() + 2) / 3),
                        impl_->options.flush_before_sysex)) {
        return;
    }

    const int stamp = impl_->stamp_milliseconds(sample_offset);
    if (impl_->options.event_delay_blocks > 0 || stamp > 0) {
        impl_->pending_sysex.push_back(
            Impl::PendingSysex{impl_->block_index + stamp + impl_->options.event_delay_blocks + 1,
                               port,
                               std::vector<std::uint8_t>{bytes.begin(), bytes.end()}});
        return;
    }
    impl_->dispatch_sysex(port, bytes);
}

void ToneGenerator::Impl::dispatch_sysex(int port, std::span<const std::uint8_t> bytes)
{
    // Universal master volume: F0 7F 7F 04 01 ll mm F7.
    if (bytes.size() >= 8 && bytes[0] == 0xF0 && bytes[1] == 0x7F && bytes[3] == 0x04
        && bytes[4] == 0x01) {
        for (Part& part : parts) {
            part.set_master(bytes[6]);
        }
        return;
    }

    // Universal non-realtime General MIDI mode: F0 7E 7F 09 xx F7. GM System On (01, and the GM2
    // form 03) resets the stream state; GM System Off (02) is recognised and left alone -- the
    // module treats it as a switch to its native map, which this engine expresses through
    // `ToneGeneratorOptions::map` instead.
    if (bytes.size() >= 6 && bytes[0] == 0xF0 && bytes[1] == 0x7E && bytes[3] == 0x09) {
        if (bytes[4] == 0x01 || bytes[4] == 0x03) {
            // A GM reset leaves XG mode as well as resetting, and `leave_xg_mode` already resets,
            // so the two must not both run.
            if (xg_mode) {
                leave_xg_mode();
            } else {
                stream_reset();
            }
        } else if (bytes[4] == 0x02) {
            leave_xg_mode();
        }
        return;
    }

    // Yamaha XG: F0 43 1n 4C <3-byte address> <data...> F7.
    if (bytes.size() >= 8 && bytes[0] == 0xF0 && bytes[1] == 0x43) {
        xg_sysex(bytes);
        return;
    }

    // Roland GS: F0 41 dev 42 <command> <3-byte address> <data...> checksum F7.
    if (bytes.size() < 11 || bytes[0] != 0xF0 || bytes[1] != 0x41 || bytes[3] != 0x42) {
        return;
    }

    // Any Roland message ends XG mode, before it is itself acted on. The module does this without
    // inspecting the message at all, so a file that interleaves the two dialects does not layer
    // them -- it flips the instrument back and forth, resetting every part each time.
    leave_xg_mode();

    // RQ1 asks for a dump, and the engine has no MIDI output to answer on.
    if (bytes[4] != 0x12) {
        return;
    }

    // The checksum folds the address and data to a multiple of 128, and the engine
    // (`sysex_receive_parse`) drops the message when it does not.
    unsigned int sum = 0;
    for (std::size_t i = 5; i + 1 < bytes.size(); ++i) {
        sum += bytes[i];
    }
    if (sum % 0x80 != 0) {
        return;
    }

    const int a1 = bytes[5];
    const int a2 = bytes[6];
    const int a3 = bytes[7];
    const std::span<const std::uint8_t> data = bytes.subspan(8, bytes.size() - 10);
    if (data.empty()) {
        return;
    }
    const int value = data[0];

    // System mode set: 00 00 7F selects mode 1 or 2, and either way arrives as a full reset.
    if (a1 == 0x00 && a2 == 0x00 && a3 == 0x7F) {
        stream_reset();
        return;
    }

    // The `50`/`51` blocks are the second port's patch and drum-setup mirrors: same layout, port
    // B parts, whatever port the message arrived on.
    const int block_port = (a1 == 0x50 || a1 == 0x51) ? 1 : (port & (port_count - 1));

    if (a1 == 0x40 || a1 == 0x50) {
        if (a2 == 0x00) {
            // System common.
            switch (a3) {
            case 0x00:
                // Master tune, four nibble bytes; clamped as `sysex_master_tune` clamps it.
                if (data.size() >= 4) {
                    const int raw =
                        ((data[0] * 0x100 + data[2]) * 0x10) + (data[1] * 0x100) + data[3];
                    master_tune = std::clamp(raw, 0x18, 0x7E8);
                }
                break;
            case 0x04:
                for (Part& part : parts) {
                    part.set_master(value);
                }
                break;
            case 0x05:
                // Master key shift, clamped to +-24 semitones (`sysex_master_key_shift`).
                master_key_shift = std::clamp(value, 0x28, 0x58);
                break;
            case 0x06:
                // Latched but not yet consumed -- the mixer has no master pan stage.
                master_pan = value;
                break;
            case 0x7F:
                // GS reset (00). The other defined value, 7F, exits GS mode; both return the
                // stream state to power-on.
                stream_reset();
                break;
            default:
                break;
            }
            return;
        }

        if (a2 == 0x01) {
            // Patch common: the effect macros with their engine-checked ranges, and the parameter
            // blocks behind them.
            if ((a3 == 0x30 || a3 == 0x31) && value <= 7) {
                reverb_type =
                    options.reverb_type ? options.reverb_type : std::optional{value};
                load_macro_rows();
            } else if (a3 == 0x38 && value <= 7) {
                chorus_type =
                    options.chorus_type ? options.chorus_type : std::optional{value};
                load_macro_rows();
            } else if (a3 == 0x50 && value <= 9) {
                delay_type =
                    options.delay_type ? options.delay_type : std::optional{value};
                // Selecting a macro reloads its level, the way the other two macros reload theirs.
                // All ten stored rows carry 64, so this only ever restores unity -- but it restores
                // it, which matters to a stream that edits the level and then changes type.
                load_delay_row();
            } else if (a3 >= 0x51 && a3 <= 0x5A) {
                // The delay parameter block, into the row the macro loaded. An edit that arrives
                // before any macro has been selected still has to land on something, so the row is
                // filled from the current type first rather than being written into zeroes.
                if (!delay_row_loaded) {
                    load_delay_row();
                }
                delay_row[static_cast<std::size_t>(a3 - 0x51)] = value;
                if (a3 == 0x58) {
                    // The wet level and the send into the reverb are matrix coefficients rather
                    // than tap gains, so they move a ramp instead of recompiling the network.
                    delay_level = value;
                } else if (a3 == 0x5A) {
                    delay_to_reverb_level = value;
                } else {
                    delay_row_edited = true;
                }
            } else if (a3 >= 0x31 && a3 <= 0x37) {
                // Reverb parameters, into the row the macro filled in -- and if no macro has been
                // selected yet, into the row the *current* macro would have filled in.
                //
                // Without the seed an edit lands in a zeroed array and `reverb_row_edited` then
                // sends that array to be compiled, so one byte of intent replaces a whole macro
                // with silence-adjacent nonsense. Nothing selects a macro at reset; the type is
                // simply the power-on default, and a stream is entitled to assume it is there.
                // Roland's own demonstration disks assume exactly that: `roland_allstars`,
                // `roland_sc55_demo13`, `roland_sc88_y03` and `roland_sc88_y05` all edit a row
                // before ever selecting one. Level is byte [2] and is kept out of the network --
                // see `reverb_level`.
                if (!reverb_row_loaded) {
                    load_reverb_row();
                }
                reverb_row[static_cast<std::size_t>(a3 - 0x31)] =
                    static_cast<std::uint8_t>(value);
                if (a3 == 0x33) {
                    reverb_level = value;
                } else {
                    reverb_row_edited = true;
                }
            } else if (a3 >= 0x39 && a3 <= 0x40) {
                // The same seed, for the same reason -- see the reverb branch above.
                if (!chorus_row_loaded) {
                    load_chorus_row();
                }
                chorus_row[static_cast<std::size_t>(a3 - 0x39)] =
                    static_cast<std::uint8_t>(value);
                if (a3 == 0x3A) {
                    chorus_level = value;
                } else if (a3 == 0x3F) {
                    chorus_to_reverb_level = value;
                } else if (a3 == 0x40) {
                    chorus_to_delay_level = value;
                } else {
                    chorus_row_edited = true;
                }
            }
            // Recognised and left alone, and one of these was checked rather than assumed.
            //
            // **Voice reserve (10-1F) is inert on the module too.** An address census over 131,997
            // archive files found 2,641 of them setting it -- more than send anything else this
            // engine ignores -- and it is the parameter that decides which part keeps its notes
            // when the 64 voices run out, so it looked like the most valuable thing on the list.
            // It is not: seven shapes of the message rendered through the DLL, on a file that
            // provably steals, all produce **byte-identical audio** to sending nothing.
            // Block-ordered 64/0 and 0/64, a flat 4 to every part, 64 to the first byte, and
            // sixteen single-byte writes -- inert, every one. So this is the module's behaviour
            // rather than a gap, and files that set it are asking for something the SC-VA does not
            // provide.
            //
            // The patch name (00-0F) has no audible counterpart. All three parameter blocks are
            // now live: reverb (32-37), chorus (39-40) and delay (51-5A) each go into the row their
            // macro loaded and recompile the network, except for the four addresses that are not
            // network coefficients at all -- the three wet levels (33, 3A, 58) and the three sends
            // between networks (3F, 40, 5A), which move a ramp in the mixer instead.
            return;
        }

        if (a2 == 0x02) {
            // The four-band EQ block (`sysex_eq_params`): low frequency and gain, then high. Each
            // setter applies the engine's own range test and ignores anything outside it.
            switch (a3) {
            case 0x00:
                equalizer.set_low_frequency(value);
                break;
            case 0x01:
                equalizer.set_low_gain(value);
                break;
            case 0x02:
                equalizer.set_high_frequency(value);
                break;
            case 0x03:
                equalizer.set_high_gain(value);
                break;
            default:
                break;
            }
            return;
        }

        if (a2 == 0x03) {
            // Insertion EFX (`sysex_insertion_fx_type` / `sysex_insertion_fx_params`): 00-01 pick
            // the type — the MSB is latched and the LSB write commits the pair, which also loads
            // the type's defaults — 03-16 are its twenty parameters, 17-19 the block's common
            // send levels, and 1B-1E the control assignments. A DT1 spanning several addresses
            // walks them a byte at a time, the way the engine's per-address state machine does.
            if (!options.efx) {
                return;
            }
            ensure_efx();
            int address = a3;
            for (const std::uint8_t byte : data) {
                if (address == 0x00) {
                    efx_type_msb = byte;
                } else if (address == 0x01) {
                    efx->select_type(efx_type_msb, byte);
                } else if (address >= 0x03) {
                    efx->set_parameter(address, byte);
                }
                ++address;
            }
            return;
        }

        if ((a2 & 0xF0) == 0x10) {
            // Part parameters, port-relative block addressing.
            const int index =
                part_of(block_port, sequence_builder::channel_from_block(a2 & 0x0F));
            gs_part_parameter(
                index, parts[static_cast<std::size_t>(index)], a3, data);
            return;
        }

        if ((a2 & 0xF0) == 0x40) {
            // The extended part block. `20` switches this part through the EQ; `22` routes it
            // through the insertion EFX instead of the dry mix.
            const int index =
                part_of(block_port, sequence_builder::channel_from_block(a2 & 0x0F));
            Part& part = parts[static_cast<std::size_t>(index)];
            if (a3 == 0x00) {
                // `sysex_part_bank_msb` writes `part+0x44d` with no clamp -- the same byte the bank
                // MSB and the mode resets set, which is the tone *space* rather than the map (XG
                // System On puts 0x77 there on every part, GM2 On 0x7a). Measured with a sweep of
                // the whole `40 4x` block against a part dump: this address and `01` are the only
                // two in it that move either byte.
                if (part.rx.bank_msb) {
                    part.bank = value;
                }
            } else if (a3 == 0x01) {
                // `sysex_part_bank_lsb` writes `part+0x44e` **clamped to 1-4**, and that is the
                // tone map. Anything outside the range is dropped rather than stored, so a zero
                // leaves the map where it was instead of returning it to the default.
                //
                // This is the same destination CC#32 reaches, and it is how every tier 2 fixture
                // in this repository selects its map -- the `scdec` harness's `ToneMap0` sends
                // exactly this message to all sixteen blocks after a GS reset.
                //
                // **The vintage is a default, not a ceiling.** The two writers do not limit each
                // other and the last one wins, measured both ways round on the module: this SysEx
                // then CC#32 = 4 renders as map 4, CC#32 = 4 then this SysEx renders as map 1. So a
                // player that injects the map once after a reset is stating a preference, and any
                // later bank LSB in the file overrides it.
                if (value >= 1 && value <= 4) {
                    part.bank_lsb = value;
                }
            } else if (a3 == 0x20) {
                part.eq_enabled = value != 0;
            } else if (a3 == 0x38) {
                // The per-program half of the hold-clock bias (`part+0x45b`). The tone loader
                // fills the eight slots before it from the tone and forces this one back to
                // centre, so unlike `40 1x 38` it does not survive a program change.
                part.envelope_delay_tone = value;
            } else if (a3 == 0x22 && options.efx) {
                part.efx_enabled = value != 0;
                if (part.efx_enabled) {
                    ensure_efx();
                }
            }
            return;
        }

        if ((a2 & 0xF0) == 0x20) {
            // The controller assignment matrix (`sysex_part_control_matrix`). The address splits
            // into a source in the high nibble and a destination in the low one.
            const int index =
                part_of(block_port, sequence_builder::channel_from_block(a2 & 0x0F));
            Part& part = parts[static_cast<std::size_t>(index)];

            const int source = a3 >> 4;
            const int destination = a3 & 0x0F;
            if (source >= ControlMatrix::source_count
                || destination >= ControlMatrix::destination_count) {
                return;
            }

            // Bend's pitch depth is the bend range, in the engine and so here: one byte, written
            // by this message and by RPN 00/00 alike, clamped to 0-24 semitones by both.
            if (source == static_cast<int>(ControlMatrix::Source::bend)
                && destination == static_cast<int>(ControlMatrix::Destination::pitch)) {
                part.bend_range = std::clamp(value - 0x40, 0, 24);
                return;
            }

            part.control.store(static_cast<ControlMatrix::Source>(source),
                               static_cast<ControlMatrix::Destination>(destination),
                               value);
            return;
        }
        return;
    }

    // `49` carries 2,738 file-hits in the same shape as the bulk dump below, writes nothing to the
    // part array, and its destination is still unidentified.
    // The patch bulk dump. `48` is port A and `58` its port-B mirror, the same `0x10` bit that
    // separates `40` from `50`.
    if (a1 == 0x48 || a1 == 0x58) {
        gs_bulk_dump(block_port, (a2 << 8) | a3, data);
        return;
    }

    // The drum-set bulk dump. `sysex_drumset_dump_dispatch` handles this and `41` alike -- it is
    // the same per-key data, arriving a plane at a time instead of a parameter at a time. There is
    // no `59` mirror: the address table's search reaches `0x7f` before it reaches `0x59`, so a
    // port-B form would be skipped, and this engine takes the port from the message instead.
    if (a1 == 0x49) {
        gs_drum_bulk_dump(block_port, a2, data);
        return;
    }

    if (a1 == 0x41 || a1 == 0x51) {
        // Drum setup: a2 is (map << 4) | parameter, a3 the first key. The map nibble picks which
        // of the two per-map setup buffers the module writes -- bit 12 of the address index, so
        // only its low bit counts -- and parts read the buffer of the map they are assigned to.
        gs_drum_setup(block_port, (a2 >> 4) & 1, a2 & 0x0F, a3, data);
        return;
    }
}

namespace {

/// Where a bulk-dump address anchors the walk: a part, a record offset, or only one of the two.
///
/// Transcribed from `sysex_dt1_addr_dispatch @ 180079a20`, and where that function reaches a
/// handler this engine has no equivalent for, from `scdec bulkmap` -- which sends a page carrying
/// its own position index and reads the destination off the part array. `48 01 00` is the second
/// kind: the dispatcher hands it to `system_set_block_then_partmap`, and what it observably does is
/// anchor part 0 at `+0x3cc`. It is also the address every real dump starts its parts with, so
/// leaving it out drops the whole walk at the first message.
struct BulkAnchor {
    int address;
    int block;   ///< -1 keeps the part the walk is already on.
    int offset;
};

/// The region starts. The offset cycles 0x40c, 0x41c, 0x42c, 0x43c as the part advances, which is
/// the same four-parts-per-group shape the transition strides show.
constexpr std::array<BulkAnchor, 24> bulk_anchors{{
    {0x0100, 0, 0x3cc},  {0x0110, 0, 0x3d4},  {0x0200, 0, 0x40c},  {0x0300, -1, 0x3dc}, {0x0400, 1, 0x41c},
    {0x0500, -1, 0x3ec}, {0x0600, 2, 0x42c},  {0x0800, 3, 0x43c},  {0x0900, 4, 0x40c},
    {0x0b00, 5, 0x41c},  {0x0c00, -1, 0x3ec}, {0x0d00, 6, 0x42c},  {0x0f00, 7, 0x43c},
    {0x1000, 8, 0x40c},  {0x1100, -1, 0x3dc}, {0x1300, 9, 0x41c},  {0x1400, 10, 0x42c},
    {0x1500, -1, 0x3fc}, {0x1600, 11, 0x43c}, {0x1700, 12, 0x40c}, {0x1950, 13, 0x41c},
    {0x1a00, -1, 0x3ec}, {0x1c00, 14, 0x42c}, {0x1d00, 15, 0x43c},
}};

/// The addresses at which `part_param_write_all` advances to the next part itself.
///
/// A stride of 0x1e0 inside a group of four parts and 0x160 between groups. These are mid-walk, so
/// the dispatcher above leaves them alone and the byte writer does the work: the byte carries the
/// next part's bank MSB and the cursor restarts at that part's `+0x3d5`.
/// Where a part's program change fires: its transition address plus ten.
///
/// **Part 0 has none, and that is the binary's own asymmetry rather than an omission here.** The
/// lowest case in `part_param_write_all`'s switch is `0x270`, part 1's transition; nothing exists
/// around `0x110`, which is where part 0's would be. A part reached by an *anchor* rather than by a
/// transition gets plain writes for the whole of its record, so a dump restores part 0's mixer but
/// never re-selects its patch. `wwtbam.mid` is the case that says so: it sends only `48 00 10` and
/// `48 01 10`, and firing a program change on part 0 from it costs four assertions against the
/// module that not firing one does not.
constexpr int bulk_program_fire_offset = 0x0a;

constexpr std::array<int, 15> bulk_transitions{
    0x0270, 0x0450, 0x0630, 0x0810, 0x0970, 0x0b50, 0x0d30, 0x0f10,
    0x1070, 0x1250, 0x1430, 0x1610, 0x1770, 0x1950, 0x1b30,
};

/// A record offset's GS part-parameter address, or -1 for one this engine must not write.
///
/// The walk is over the module's own part struct, so the map is the inverse of `scdec partmap`,
/// which writes every `40 1x pp` and reports the byte it moved. Two kinds of offset are left at -1
/// and both matter.
///
/// The first is a record byte no part-parameter address reaches, which is most of `0x40c`-`0x443`.
/// That is a **decision, not a gap**. It was recorded here as "reached by the extended `40 4x`
/// block instead"; sweeping that block with `scdec partmap` says otherwise -- it lands on `+0x44d`,
/// `+0x44f`, `+0x453`-`+0x45b`, `+0x46e` and `+0x471`, nowhere near. Searched in the binary, only
/// fifteen of those ~58 offsets are referenced *anywhere at all*, and every one of the fifteen is
/// read by `lfo_update`, `lfo2_update`, `mod_pitch_control`, `tvf_cutoff_add_lfo` or
/// `voice_volume_apply`: running modulation state, recomputed each control tick. The rest are never
/// read by anything. A dump carries them and they are correctly ignored -- `darkness3.mid` fills
/// the whole stretch with `40 00 00 00` repeating, which is that structure at its neutral setting.
///
/// The second is a byte the block *does* address but does not own outright -- a field inside a byte
/// rather than the byte. `partmap` reports `40 1x 02` moving `+0x3d8` from `00` to `20` when it
/// wrote `33`, and `40 1x 14` moving `+0x3d9` from `81` to `82`. Four of these exist, and having
/// decoded them the answer is that only one carries anything this engine can use:
///
/// - `+0x3d8` is `(port << 4) | rx channel`. **Handled in the walk**, unpacked rather than dropped.
/// - `+0x3d9` holds the assign mode in bits 0-1, use-for-rhythm at 0x10 and the kit slot at 0x20.
///   The rhythm bits are read where the program change fires; the assign mode reaches a placeholder
///   here, because this voice pool has one allocation policy, so mapping it would change nothing.
/// - `+0x446` (`40 1x 26`) the engine stores as a boolean and **never reads again** -- the offset
///   appears exactly once in the whole binary, at that write.
/// - `+0x44c` (`40 1x 60`) has no handler in this engine at all.
///
/// So the remaining two are dropped because they are inert, not because they are unknown. `partmap` reports
/// `40 1x 02` moving `+0x3d8` from `00` to `20` when it wrote `33`, and `40 1x 14` moving `+0x3d9`
/// from `81` to `82` -- both **packed**, a field inside a byte rather than the byte. Copying a
/// dump's raw byte through the parameter handler sets the whole thing, and `+0x3d8`/`+0x3d9` carry
/// the Rx channel and the melodic/drum selector: `darkness3.mid` came out with every melodic part
/// deaf. `+0x446` (`40 1x 26`) and `+0x44c` (`40 1x 60`) are packed the same way. Until the packing
/// is decoded these are dropped rather than half-applied.
[[nodiscard]] constexpr int bulk_parameter_of(int offset) noexcept
{
    if (offset >= 0x3e4 && offset <= 0x3eb) {
        return 0x30 + (offset - 0x3e4);   // the eight tone modifiers
    }
    if (offset >= 0x3ee && offset <= 0x3f9) {
        return 0x40 + (offset - 0x3ee);   // the twelve scale-tuning bytes
    }
    switch (offset) {
    case 0x3d4: return 0x00;   // bank MSB; one byte, so no program change follows it
    case 0x3da: return 0x16;   // key shift
    case 0x3dc: return 0x19;   // part level
    case 0x3dd: return 0x1C;   // panpot
    case 0x3de: return 0x1A;   // velocity sense depth
    case 0x3df: return 0x1B;   // velocity sense offset
    case 0x3e0: return 0x1D;   // key range low
    case 0x3e1: return 0x1E;   // key range high
    case 0x3e2: return 0x21;   // chorus send
    case 0x3e3: return 0x22;   // reverb send
    case 0x3fa: return 0x1F;   // CC1 controller number
    case 0x3fb: return 0x20;   // CC2 controller number
    case 0x44a: return 0x2C;   // delay send
    case 0x44b: return 0x38;   // the ninth tone-modify slot
    case 0x47c: return 0x50;
    default: return -1;
    }
}

/// The last offset the linear walk covers before it moves to the next part.
///
/// The last offset a part's record reaches, past which the walk has lost sync.
///
/// A transition leaves the cursor at `+0x3d5` and the next one falls 112 values later, so a record
/// runs `+0x3d4` (the bank the transition itself writes) through `+0x443`. That 112 is not fitted:
/// converted into value positions, every one of the fifteen transition addresses sits exactly 112
/// apart, because `a3` only runs `0x00`-`0x7e` and each 64-value message fills one `a2` exactly.
/// With page 29's four bytes per part on top it is 116, the size of the GS part block, which is
/// where the page arithmetic started.
///
/// This is a **guard, not an advance**. See the walk.
constexpr int bulk_record_end = 0x443;

} // namespace

void ToneGenerator::Impl::gs_bulk_dump(int block_port,
                                       int address,
                                       std::span<const std::uint8_t> data)
{
    // Roland's ordinary nibble packing: two data bytes per value, high nibble first. Real files
    // send 128 bytes for 64 values, and every byte is 0x0-0xf -- which is how the shape was first
    // recognised in the archive, and what a raw-versus-packed probe against the module confirmed.
    if (data.size() % 2 != 0) {
        return;
    }
    for (const std::uint8_t byte : data) {
        if (byte > 0x0F) {
            return;
        }
    }

    // Anchor, if this address is one. An address in neither table continues the walk -- and if no
    // walk has been established, there is nothing to continue and the message is dropped.
    for (const BulkAnchor& anchor : bulk_anchors) {
        if (anchor.address != address) {
            continue;
        }
        if (anchor.block >= 0) {
            bulk_part_block = anchor.block;
        }
        bulk_record_offset = anchor.offset;
        break;
    }
    if (bulk_part_block < 0 || bulk_record_offset < 0) {
        return;
    }

    const std::size_t count = data.size() / 2;
    for (std::size_t i = 0; i < count; ++i) {
        const int value = (data[i * 2] << 4) | data[(i * 2) + 1];

        // **The address advances once per payload byte, not once per value.** `g_sysex_addr_idx` is
        // incremented by the receive loop, which sees nibbles, so a value spans two addresses.
        // Halving the address axis puts the walk out of phase with every per-part special, which is
        // what a trace across a real dump shows. What settles it: `scdec bulkmap` puts page 2's
        // part transition at payload position 56, and `0x200 + 2*56` is `0x270` -- part 1's
        // transition address to the byte.
        const int here = address + (2 * static_cast<int>(i));

        if (std::find(bulk_transitions.begin(), bulk_transitions.end(), here)
            != bulk_transitions.end()) {
            // The walk moves to the next part and this byte is its bank MSB. Past part 15 there is
            // no next part; the module would carry on into whatever follows the array.
            if (bulk_part_block >= 15) {
                bulk_part_block = -1;
                bulk_record_offset = -1;
                return;
            }
            ++bulk_part_block;
            bulk_record_offset = 0x3d5;

            // The byte is the new part's bank MSB, stored rather than acted on. The program
            // follows at `+0x3d5` and the change fires at `+0x3d9`, which is also where the record
            // says whether this is a drum part -- see `bulk_pending_program`. Any program the
            // previous part left unfired belongs to that part and is dropped here rather than
            // landing on this one.
            const int index =
                part_of(block_port, sequence_builder::channel_from_block(bulk_part_block));
            parts[static_cast<std::size_t>(index)].bank = value;
            bulk_pending_program = -1;
            continue;
        }

        // Running past a record's end is desync, not a part advance. **The transitions do all the
        // advancing** -- they fall every 112 values without exception, which is what a part's record
        // is worth in the walk -- so a second mechanism here would advance twice and skip a part.
        // That is exactly what it did: traced across a real dump, two blocks took their level twice
        // and two never took it at all.
        //
        // The module bounds-checks nothing on this path and would keep writing into whatever
        // follows its array. Drop the walk instead, so a later continuation cannot resume from a
        // position that no longer means anything.
        if (bulk_record_offset > bulk_record_end) {
            bulk_part_block = -1;
            bulk_record_offset = -1;
            return;
        }

        const int offset = bulk_record_offset;
        ++bulk_record_offset;

        const int index =
            part_of(block_port, sequence_builder::channel_from_block(bulk_part_block));
        Part& part = parts[static_cast<std::size_t>(index)];

        // The program is carried at `+0x3d5` and acted on later, by address rather than by
        // offset -- only a part the walk *transitioned* into has a case that fires it.
        if (offset == 0x3d5) {
            bulk_pending_program = value & 0x7F;
            continue;
        }

        // The Rx channel, unpacked. `+0x3d8` is not the channel, it is `(port << 4) | channel` --
        // the engine builds it as `table[part] * 0x10 + value` from a parameter change that carries
        // only the low nibble, and a dump carries the assembled byte. Routing the whole byte through
        // the parameter handler sets the channel to a port index and the part stops hearing
        // anything: `darkness3.mid` sounded 771 of its 3,607 notes that way. The high nibble is the
        // port, which this engine takes from the message rather than from the record.
        if (offset == 0x3d8) {
            part.rx_channel = value & 0x0F;
            continue;
        }

        const bool fires_program =
            std::any_of(bulk_transitions.begin(), bulk_transitions.end(),
                        [here](int t) { return t + bulk_program_fire_offset == here; });
        if (fires_program) {
            // The same byte says whether this is a drum part: bit 0x10 is use-for-rhythm and 0x20
            // picks which of the two kit slots. The engine folds them into the low five bits of
            // `part+0x12` and only then calls the melodic or the drum loader, so the selector has
            // to be in place before the program resolves. `darkness3.mid` carries 0xb0 here for
            // block 0 -- channel 10 -- which is both bits, and block 0 is the drum part.
            // Bit 0x10 is use-for-rhythm, and **bit 0x40 is the map** -- not 0x20, which is what
            // this read first. The two are separate flags rather than one selector, and reading
            // the wrong one swaps MAP1 and MAP2.
            //
            // Measured with `scdec smfstate`. After a bare GS reset block 0 carries `0xb1`. Send
            // `40 14 15 = 01` and block 4 also becomes `0xb1`, selector `0x20`, and a program
            // change on it writes through to block 0 -- MAP1 shares the drum channel's kit slot.
            // Send `= 02` instead and block 4 becomes `0xd1`, selector `0x21`, with block 0 left
            // alone. So `0xb1` is 1 and `0xd1` is 2, and the differing bit is 0x40.
            //
            // `darkness3.mid` is where it showed: its dump puts blocks 0 to 3 on `0xb1` and block 4
            // on `0xd1`. Read through 0x20 those came out swapped, which put block 4 on the same
            // slot as the default drum channel -- so block 4's program 56 landed in it and channel
            // 10 sounded the SFX kit where the module has Standard 2.
            part.rhythm = (value & 0x10) == 0 ? 0 : ((value & 0x40) != 0 ? 2 : 1);
            if (bulk_pending_program >= 0) {
                program_change(index, part, bulk_pending_program);
                bulk_pending_program = -1;
            }
            continue;
        }

        const int parameter = bulk_parameter_of(offset);
        if (parameter < 0) {
            continue;
        }
        const std::array<std::uint8_t, 1> one{static_cast<std::uint8_t>(value)};
        gs_part_parameter(index, part, parameter, one);
    }
}

void ToneGenerator::Impl::gs_part_parameter(int part_index,
                                            Part& part,
                                            int address,
                                            std::span<const std::uint8_t> data)
{
    const int value = data[0];

    switch (address) {
    // Tone number: the bank MSB then the program, exactly a CC#0 plus program change pair.
    case 0x00:
        part.bank = value;
        if (data.size() >= 2) {
            program_change(part_index, part, data[1] & 0x7F);
        }
        break;

    case 0x02:
        // Rx channel, 0-15, or 0x10 for off. Written as-is: matching happens per message.
        part.rx_channel = std::min(value, 0x10);
        break;

    case 0x03:
        part.rx.pitch_bend = value != 0;
        break;
    case 0x04:
        part.rx.channel_pressure = value != 0;
        break;
    case 0x05:
        part.rx.program_change = value != 0;
        break;
    case 0x06:
        part.rx.control_change = value != 0;
        break;
    case 0x07:
        part.rx.poly_pressure = value != 0;
        break;
    case 0x08:
        part.rx.notes = value != 0;
        break;
    case 0x09:
        part.rx.rpn = value != 0;
        break;
    case 0x0A:
        part.rx.nrpn = value != 0;
        break;
    case 0x0B:
        part.rx.modulation = value != 0;
        break;
    case 0x0C:
        part.rx.volume = value != 0;
        break;
    case 0x0D:
        part.rx.panpot = value != 0;
        break;
    case 0x0E:
        part.rx.expression = value != 0;
        break;
    case 0x0F:
        part.rx.hold = value != 0;
        break;
    case 0x10:
        part.rx.portamento = value != 0;
        break;
    case 0x11:
        part.rx.sostenuto = value != 0;
        break;
    case 0x12:
        part.rx.soft = value != 0;
        break;

    case 0x13:
        // Mono/poly: zero is mono, like CC#126/127 collapsed to a byte.
        part.mono = value == 0;
        flush_part_voices(part_index);
        break;

    case 0x14:
        // Assign mode (`sysex_part_assign_mode`): single/limited/full voice assignment.
        // Placeholder -- the voice pool has one allocation policy.
        break;

    case 0x15:
        // Use for rhythm: 0 melodic, 1 or 2 a drum map. Re-resolves the kit so a program already
        // in force selects one, the way the engine's handler re-runs the program change.
        part.rhythm = std::min(value, 2);
        if (part.rhythm > 0) {
            program_change(part_index, part, part.program);
        }
        break;

    case 0x16:
        // Part key shift, same clamp as the master (`sysex_part_key_shift`).
        part.key_shift = std::clamp(value, 0x28, 0x58);
        break;

    case 0x17:
        // Pitch offset fine, stored raw; the module's unit is Hz, which nothing consumes yet.
        part.pitch_offset_fine = value;
        break;

    case 0x19:
        part.set_volume(value);
        break;

    case 0x1A:
        part.velocity_depth = value;
        break;
    case 0x1B:
        part.velocity_offset = value;
        break;

    case 0x1C:
        // The SysEx panpot is the one writer that can reach zero -- GS RND.
        part.pan = value;
        break;

    case 0x1D:
        part.key_low = value;
        break;
    case 0x1E:
        part.key_high = value;
        break;

    // Which Control Change each assignable matrix source listens to. GS documents the range as
    // 0-95, which stops short of the data-entry and RPN/NRPN controllers and of the channel-mode
    // messages -- pointing a matrix source at those would be pointing it at something that is not
    // a continuous amount.
    case 0x1F:
        part.cc1_number = std::clamp(value, 0, 95);
        break;
    case 0x20:
        part.cc2_number = std::clamp(value, 0, 95);
        break;

    case 0x21:
        part.chorus_send = value;
        break;
    case 0x22:
        part.reverb_send = value;
        break;

    case 0x23:
        part.rx.bank_msb = value != 0;
        break;
    case 0x24:
        part.rx.bank_lsb = value != 0;
        break;

    case 0x26:
        // Recognised and dropped, and that is exact rather than a shortcut: the engine stores this
        // one as a boolean at `part+0x446` and then never reads it. The offset appears exactly once
        // in the whole binary -- that write. No voice path, no getter, no CC, no NRPN, and it is
        // not in the published Roland list either.
        break;

    case 0x2C:
        part.delay_send = value;
        break;

    case 0x38:
        // The ninth tone-modify slot (`part+0x44b`, 0x40 neutral), continuing the `30`-`37` run
        // past where Roland's published list ends: a bias on the envelope hold clock's rate index.
        // Above neutral it *arms* a start delay on partials that have none. Survives a program
        // change, unlike its `40 4x 38` twin.
        part.envelope_delay = value;
        break;

    // The part modify offsets -- third writer onto the same bytes as CC#71-78 and the NRPNs.
    case 0x30:
        part.vibrato_rate = value;
        break;
    case 0x31:
        part.vibrato_depth = value;
        break;
    case 0x32:
        part.tvf_cutoff = value;
        break;
    case 0x33:
        part.tvf_resonance = value;
        break;
    case 0x34:
        part.env_attack = value;
        break;
    case 0x35:
        part.env_decay = value;
        break;
    case 0x36:
        part.env_release = value;
        break;
    case 0x37:
        part.vibrato_delay = value;
        break;

    default:
        // Scale tuning is written as a run: a DT1 at 40 up to twelve bytes long is the common
        // form, and single-entry writes land here too.
        if (address >= 0x40 && address <= 0x4B) {
            for (std::size_t i = 0; i < data.size() && address + static_cast<int>(i) <= 0x4B; ++i) {
                part.scale_tuning[static_cast<std::size_t>(address - 0x40) + i] = data[i];
            }
        }
        break;
    }
}

void ToneGenerator::Impl::gs_drum_bulk_dump(int block_port,
                                           int a2,
                                           std::span<const std::uint8_t> data)
{
    // Nibble-packed like the patch dump: two payload bytes a value, high nibble first.
    if (data.size() % 2 != 0 || data.size() > 256) {
        return;
    }
    for (const std::uint8_t byte : data) {
        if (byte > 0x0F) {
            return;
        }
    }

    // `a2` carries the whole addressing, and it is arithmetic rather than a table. Bit 4 picks the
    // kit slot -- the same bit `41` uses for its map. The low nibble picks a per-key plane in
    // *pairs*: the even member carries keys 0-63 and the odd member 64-127, so one plane takes two
    // messages. `0x0e` and `0x0f` are the two odd ones out.
    const int map = (a2 >> 4) & 1;
    const int low = a2 & 0x0F;

    int parameter = 0;
    int first_key = 0;
    if (low == 0x0E) {
        // The twelve-character kit name, and nothing here displays one.
        return;
    }
    if (low == 0x0F) {
        parameter = 0x09;   // delay send, unpaired
    } else {
        parameter = ((low & 0x0E) >> 1) + 1;
        first_key = (low & 1) * 64;
    }

    const auto count = static_cast<int>(data.size() / 2);

    // The module walks off the end here as it does everywhere else on this path -- `49 0e 00` starts
    // at `+0x500` in buffers spaced `0x50c`, so a full payload writes into the next one. Refuse
    // rather than reproduce.
    if (first_key + count > 128) {
        return;
    }

    std::array<std::uint8_t, 128> values{};
    for (int i = 0; i < count; ++i) {
        values[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(
            (data[static_cast<std::size_t>(i) * 2] << 4)
            | data[(static_cast<std::size_t>(i) * 2) + 1]);
    }
    const std::span<const std::uint8_t> plane{values.data(), static_cast<std::size_t>(count)};

    // Rx note-off and Rx note-on share one plane, because on the module they share one *byte*:
    // `sysex_drumset_rxnote_bit0` and `sysex_drumset_rxnote_bit4` write bit 0 and bit 4 of
    // `part+0x480`. The per-parameter form addresses them separately and this one cannot, so the
    // byte is split back out here.
    if (parameter == 0x07) {
        std::array<std::uint8_t, 128> bit{};
        for (int i = 0; i < count; ++i) {
            bit[static_cast<std::size_t>(i)] = values[static_cast<std::size_t>(i)] & 0x01;
        }
        gs_drum_setup(block_port, map, 0x07, first_key,
                      std::span<const std::uint8_t>{bit.data(), static_cast<std::size_t>(count)});
        for (int i = 0; i < count; ++i) {
            bit[static_cast<std::size_t>(i)] = values[static_cast<std::size_t>(i)] & 0x10;
        }
        gs_drum_setup(block_port, map, 0x08, first_key,
                      std::span<const std::uint8_t>{bit.data(), static_cast<std::size_t>(count)});
        return;
    }

    gs_drum_setup(block_port, map, parameter, first_key, plane);

    // **This lands and is then thrown away on the files that need it most.** `darkness3.mid` sends
    // its whole dump at tick 0 on track 0 and a program change for every channel at tick 0 on tracks
    // 2-10, including `ch10 = 0`. A drum program change resets `Part::drum_keys` here whenever the
    // kit resolves, so the dump's per-key data survives about a block. The module keeps this in a
    // buffer per *map* rather than per part, and reseeds it from the kit record only when the kit
    // actually changes -- selecting the kit already loaded reloads nothing. Which of those two is
    // right is measurable and not yet measured; see the task list.
}

void ToneGenerator::Impl::gs_drum_setup(int port,
                                        int map,
                                        int parameter,
                                        int key,
                                        std::span<const std::uint8_t> data)
{
    // The engine keeps one drum-setup buffer per map, shared by every part on that map; here the
    // overrides live on the parts, so the write lands on each of the port's rhythm parts *on the
    // addressed map*. That filter is not pedantry: files carry MAP2 setup blocks -- intro-4.mid
    // writes its entire drum setup there, per-key Rx switches included -- while their rhythm
    // part sits on the default MAP1, and on the module those writes land in a buffer no part
    // reads. Applying them anyway silenced the file's kick and snare.
    const int base = (port & (port_count - 1)) * Sequence::channel_count;
    for (int i = 0; i < Sequence::channel_count; ++i) {
        const int index = base + i;
        if (!is_drum_part(index)) {
            continue;
        }
        Part& part = parts[static_cast<std::size_t>(index)];
        if (map_of(part) != map) {
            continue;
        }
        DrumKeyOverrides& planes = drum_setup[kit_slot(index)];

        for (std::size_t offset = 0; offset < data.size(); ++offset) {
            const int note = key + static_cast<int>(offset);
            const int value = data[offset];
            switch (parameter) {
            case 0x00:
                // Drum set name -- nothing to display it on.
                break;
            case 0x01:
                // Play key number: absolute, unlike the relative pitch NRPN.
                planes.set_play_key(note, value);
                break;
            case 0x02:
                planes.set_level(note, value);
                break;
            case 0x03:
                // Assign group, stored as sent. **Not clamped**, and neither end of the old clamp
                // was harmless.
                //
                // `drum_setup_assign_group` writes the raw byte to `key + 0x400` and then keeps a
                // has-a-group bit beside it: `param_1 == 0` clears bit 3 of `key + 0x480` and any
                // other value sets it. So **zero means no group**, explicitly, and it is by far the
                // common value -- `darkness3.mid` sends 0 for 113 of its 128 keys. Clamping that up
                // to 1 put every ungrouped key into one choke group, where each drum cut the last,
                // and a kit whose keys nearly all share a group sounds like a part stuck in mono.
                //
                // The upper end mattered too: that file uses groups up to 7, and folding 5, 6 and 7
                // onto 4 made three independent groups choke each other. The comparison downstream
                // is equality against `key.group`, which is already guarded by `!= 0`, so the raw
                // byte is exactly what it wants.
                planes.set_group(note, value);
                break;
            case 0x04:
                planes.set_pan(note, value);
                break;
            case 0x05:
                planes.set_reverb(note, value);
                break;
            case 0x06:
                planes.set_chorus(note, value);
                break;
            case 0x07:
                // Rx note-off per key: bit 0 of the module's per-key flag byte, seeded by the
                // kit record and rewritten here (`drum_setup_rx_noteoff`). Nonzero engages, as
                // the module's own write does.
                planes.set_rx_note_off(note, value != 0);
                break;
            case 0x08:
                // Rx note-on per key: bit 4 of the same byte (`drum_setup_rx_noteon`). A key
                // switched off here does not sound at all -- the module's note-on dispatch
                // refuses it before velocity or mute groups are even considered.
                planes.set_rx_note_on(note, value != 0);
                break;
            case 0x09:
                planes.set_delay(note, value);
                break;
            default:
                break;
            }
        }
    }
}

void ToneGenerator::Impl::stream_reset()
{
    // What a reset message does mid-stream: every voice fades rather than being cut dead, every
    // part returns to power-on, and the effect selections fall back to the host's configuration.
    // The clocks and counters stay -- this is a message in the stream, not a host reset.
    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        auto& slot = slots[static_cast<std::size_t>(i)];
        if (slot) {
            slot->choke();
            dying.push_back(std::move(slot));
            pool.free_slot(i);
        }
    }
    pool.reset();

    for (int i = 0; i < port_count * Sequence::channel_count; ++i) {
        Part& part = parts[static_cast<std::size_t>(i)];
        part.reset();
        part.rx_channel = i % Sequence::channel_count;

        // XG parts come up with Rx NRPN *off*. `xg_system_on` sets `g_xg_mode` before it calls
        // `engine_all_parts_reset`, so the part default it resets to is the XG one: the Rx word at
        // part+0x3d6 reads 0xffff after a GS reset and 0x7fff after XG System On, on every part
        // rather than only the drum ones.
        //
        // The whole GS NRPN set goes with it -- the eight modify offsets at `01 xx` and all six
        // drum planes -- which is why an XG file that also sends GS drum NRPNs gets nothing from
        // them. Only `08 nn 37`, Rcv NRPN, turns the route back on.
        if (xg_mode) {
            part.rx.nrpn = false;
        }
    }

    reverb_type = options.reverb_type;
    chorus_type = options.chorus_type;
    delay_type = options.delay_type;

    for (StandingLfoNodes& nodes : standing_lfo) {
        nodes.clear();
    }
    master_tune = 0x400;
    master_key_shift = 0x40;
    master_pan = 0x40;
    std::fill(drum_kit.begin(), drum_kit.end(), 0);
    for (DrumKeyOverrides& planes : drum_setup) {
        planes.reset();
    }

    // The EQ returns to flat and drops its filter memory with it. Keeping the memory would only
    // matter if a later message switched the EQ back on, and what it would then contribute is one
    // block of a signal from before the reset -- so dropping it is both simpler and righter.
    equalizer.reset();

    // The insertion EFX returns to power-on: dropped rather than reset, and rebuilt at Thru with
    // its defaults the next time a stream touches it.
    efx.reset();
    efx_type_msb = 0;
}

void ToneGenerator::send_packet(std::uint32_t packet)
{
    // The module's own mask, widened by one bit. It clears everything above the port's low bit, so
    // the class nibble survives and ports 2-15 fold onto 0 and 1 rather than indexing parts that do
    // not exist.
    constexpr std::uint32_t port_mask = 0x1F;

    const int header = static_cast<int>(packet & port_mask);
    const int status = static_cast<int>((packet >> 8) & 0xFF);
    if (status < 0x80 || status >= 0xF0) {
        // System common and realtime carry no channel, so there is no part for them to reach.
        return;
    }

    send_channel(header >> 4,
                 status,
                 static_cast<int>((packet >> 16) & 0x7F),
                 static_cast<int>((packet >> 24) & 0x7F));
}

// ---------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------

void ToneGenerator::render(std::span<float> left, std::span<float> right)
{
    if (left.size() != right.size()) {
        throw std::invalid_argument("The two channels must be the same length.");
    }

    std::size_t written = 0;
    while (written < left.size()) {
        if (impl_->block_offset >= block_size) {
            impl_->render_block();
            // TS_VOICE_COUNT: sounding voices per block, to set beside the module's own count. A
            // detuned unison losing one of its two partials is ~3 dB down, constant, with its
            // envelope intact -- the signature NativeTS #8's channel 15 shows and which nothing
            // measurable about the expression path explains.
            static const bool probe_voices = std::getenv("TS_VOICE_COUNT") != nullptr;
            if (probe_voices) {
                int live = 0;
                for (const auto& slot : impl_->slots) {
                    if (slot && !slot->finished()) {
                        ++live;
                    }
                }
                std::fprintf(stderr, "voices: %lld,%d\n",
                             static_cast<long long>(impl_->position), live);
            }
            impl_->block_offset = 0;
        }

        const auto count = std::min<std::size_t>(
            static_cast<std::size_t>(block_size - impl_->block_offset), left.size() - written);
        const auto from = static_cast<std::size_t>(impl_->block_offset);

        std::copy_n(impl_->block_left.begin() + static_cast<std::ptrdiff_t>(from),
                    count,
                    left.begin() + static_cast<std::ptrdiff_t>(written));
        std::copy_n(impl_->block_right.begin() + static_cast<std::ptrdiff_t>(from),
                    count,
                    right.begin() + static_cast<std::ptrdiff_t>(written));

        if (impl_->output_gain != 1.0) {
            simd::scale(left.subspan(written, count), impl_->output_gain);
            simd::scale(right.subspan(written, count), impl_->output_gain);
        }

        impl_->block_offset += static_cast<int>(count);
        written += count;
        impl_->position += static_cast<std::int64_t>(count);
    }
}

void ToneGenerator::Impl::render_block()
{
    // The pipeline's clock ticks with the chunk, and everything it has been holding lands before
    // any audio for this chunk is produced.
    ++block_index;
    drain_events();

    // Everything this chunk allocated now reads its parameters, part by part in block order. The
    // module's halves are a note-on handler that allocates in arrival order and a separate pass
    // that reads parameters in block order; doing either in the other's order moves every draw.
    start_pending_voices();

    // The parts' chorus and delay sends are matrix coefficients, one a part, and move on the
    // control tick like everything else. Ten blocks to a tick.
    if (block_tick == 0) {
        for (Part& part : parts) {
            part.slew_sends(Part::send_slew_per_tick);
        }
    }
    block_tick = (block_tick + 1) % (control_block / block_size);

    std::span<float> left{block_left};
    std::span<float> right{block_right};

    std::fill(left.begin(), left.end(), 0.0F);
    std::fill(right.begin(), right.end(), 0.0F);
    reverb_bus.fill(0.0F);
    chorus_bus.fill(0.0F);
    delay_bus.fill(0.0F);
    eq_left.fill(0.0F);
    eq_right.fill(0.0F);
    if (efx) {
        efx_in_left.fill(0.0F);
        efx_in_right.fill(0.0F);
    }

    for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
        auto& slot = slots[static_cast<std::size_t>(i)];
        if (slot) {
            mix_voice(*slot, left, right);
            if (slot->finished()) {
                recycle(i);
            }
        }
    }

    for (std::size_t i = dying.size(); i-- > 0;) {
        mix_voice(*dying[i], left, right);
        if (dying[i]->finished()) {
            spare.push_back(std::move(dying[i]));
            dying.erase(dying.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // The EQ'd parts are filtered as one block and folded back into the dry mix. Summing rather
    // than having written straight through costs nothing when the bus is idle: the buffers are
    // cleared to +0 and adding +0 to a float leaves it exactly as it was. Parts reset with the
    // switch **on**, so the bus is normally busy and it is `Equalizer::is_flat` that keeps this
    // free -- a stream that never programs `40 02` renders identically to one compiled without any
    // of this, because the reset block is exactly unity.
    equalizer.process(eq_left, eq_right);
    for (int n = 0; n < block_size; ++n) {
        left[static_cast<std::size_t>(n)] += eq_left[static_cast<std::size_t>(n)];
        right[static_cast<std::size_t>(n)] += eq_right[static_cast<std::size_t>(n)];
    }

    // The insertion EFX runs ahead of the send network: its stereo output joins the dry mix, and
    // its mono sum feeds the reverb, chorus and delay buses at the block's own `40 03 17`-`19`
    // levels — the sends the EFX parts themselves were denied.
    if (efx) {
        efx->process(efx_in_left, efx_in_right, efx_out_left, efx_out_right);
        const auto to_reverb = static_cast<float>(reverb ? efx->reverb_send() : 0.0);
        const auto to_chorus = static_cast<float>(chorus ? efx->chorus_send() : 0.0);
        const auto to_delay = static_cast<float>(delay ? efx->delay_send() : 0.0);
        for (int n = 0; n < block_size; ++n) {
            const auto i = static_cast<std::size_t>(n);
            left[i] += efx_out_left[i];
            right[i] += efx_out_right[i];
            const float mono = efx_out_left[i] + efx_out_right[i];
            if (to_reverb != 0.0F) {
                reverb_bus[i] += mono * to_reverb;
            }
            if (to_chorus != 0.0F) {
                chorus_bus[i] += mono * to_chorus;
            }
            if (to_delay != 0.0F) {
                delay_bus[i] += mono * to_delay;
            }
        }
    }

    mix_effects(left, right);

    // The module's output stage, over the finished chunk. `TG_Process` runs it on everything it
    // emits, at every host rate including the engine's own -- see `OutputFilter`.
    if (!options.bypass_output_filter) {
        for (int n = 0; n < block_size; ++n) {
            const auto i = static_cast<std::size_t>(n);
            const auto [l, r] = output_filter.process(left[i], right[i]);
            left[i] = l;
            right[i] = r;
        }
    }
}

void ToneGenerator::Impl::mix_voice(PartialVoice& voice,
                                    std::span<float> left,
                                    std::span<float> right)
{
    const Part& part = parts[static_cast<std::size_t>(voice.channel())];
    std::span<float> block{scratch};

    // The static tune rides in with the bend: the engine folds RPN and key-shift tuning into one
    // per-part milli-semitone offset added to every voice's pitch, and the master tune and key
    // shift sit on top of that globally.
    // Bend is inside the matrix now, not beside it: the engine sums the five matrix sources and
    // clamps the total, so applying bend separately would escape that clamp.
    //
    // All eleven destinations come out of one call. Ten of them are the voice's business rather
    // than the mix's, so they travel into `render` together; pitch is read off here because the
    // part's static tune and the master tune are summed with it before the voice ever sees it.
    const ControlMatrix::Modulation matrix = part.matrix(part.key_pressure(voice.note()));
    const double pitch_offset =
        part.tune_milli_semitones() + matrix.pitch + master_tune_milli_semitones();
    // Refreshed per block, not latched at note-on: a filter sweep has to reach notes that are
    // already sounding. Both halves of the filter move this way -- the engine's stage A re-reads
    // the part's cutoff and resonance bytes on every control tick.
    const PartModifiers modifiers = part.modifiers();
    voice.set_cutoff_offset(modifiers.cutoff_offset());
    voice.set_part_resonance(modifiers.tvf_resonance);

    // Ahead of `render`, which advances the sample clock the retarget tick is found from. The ramp
    // runs whether or not the part is audible, so muting cannot leave it stranded mid-glide.
    voice.volume_gains(part.volume_word(), volume_ramp_mask, volume_gains);
    voice.slew_pan(part.pan);
    voice.slew_reverb_send(part.reverb_send);

    voice.render(block, pitch_offset, matrix);

    // A silenced part contributes nothing at all -- not to the dry mix and not to the sends
    // either, so muting a part also removes its tail. It keeps running, so unmuting is instant.
    //
    // Indexed by **part**, not by channel. The mask was sixteen wide once and the fold that made it
    // fit meant muting channel 3 silenced it on every port, which is one strip standing for two
    // parts that have nothing to do with each other -- different programs, different volumes, and
    // on a multi-port file different music.
    if (options.channels != nullptr && !options.channels->is_audible(voice.channel())) {
        return;
    }

    const auto [pan_left, pan_right] = voice.pan_gains();

    // The part fader is a *buffer* now, not a scalar: `voice_ctrl_ramp_b` glides it toward each new
    // CC#7/CC#11 value over about 6 ms instead of stepping it, which is what keeps a volume slide
    // from clicking once per event.
    //
    // The kit level is folded into it rather than multiplied alongside, so every mix below keeps the
    // two-multiply shape it had when the fader was a scalar -- `(sample * gain) * pan`, with the
    // per-sample gain standing exactly where the constant one did. The engine folds the kit level
    // into the ramped word itself; it is fixed for the life of a strike, so the product agrees.
    if (const double level = voice.level_gain(); level != 1.0) {
        for (double& gain : volume_gains) {
            gain *= level;
        }
    }
    std::span<const double> gains{volume_gains};

    // An EFX part detours to the block's input pair and has both sends forced to the null bus,
    // exactly as the voice bus-assign does with `part+0x452` set — the block's own send levels
    // are what reach the system effects. The EFX routing wins over the EQ detour.
    const bool efx_part = efx && part.efx_enabled;

    // Sends are fed from the pre-pan mono and are post-fader, so the wet scales with volume and
    // expression but not with pan -- matching the engine's mono send bus.
    // Read off the voice's slewed levels, not the part's targets: a send that has just been turned
    // down is still feeding the bus on its way there, and one just turned up is not yet at full.
    // The per-key depth multiplies the part's send rather than replacing it, on all three
    // networks. Measured one process per cell, since a GS reset does not isolate them:
    //   reverb, per-key x CC#91, tail   0.0006534 0.0013053 / 0.0013053 0.0025893
    //   delay,  per-key x CC#94, tail   0.0023021 0.0046040 / 0.0046040 0.0091360
    //   chorus, per-key x CC#93, hit    0.0365974 0.0441678 / 0.0441678 0.0647341
    // Each is a product: symmetric off-diagonal, doubling either input doubling the wet, and
    // silence whenever either side is 0. Chorus is read over the hit rather than after it, having
    // no tail where the other two have one. For a melodic voice every scale is 1.0, so the part's
    // send reaches the bus unchanged.
    // Named "effective" rather than "level" to keep them clear of `reverb_level` and its two
    // siblings, which are the system units' own master levels and a different quantity entirely.
    const double effective_reverb_send = voice.reverb_send() * voice.key_reverb_scale();
    const double to_reverb = !efx_part && reverb && effective_reverb_send > 0.0
                                 ? Reverb::send_gain(effective_reverb_send)
                                 : 0.0;
    const double effective_chorus_send = part.chorus_send_level() * voice.key_chorus_scale();
    const double to_chorus = !efx_part && chorus && effective_chorus_send > 0.0
                                 ? Chorus::send_gain(effective_chorus_send)
                                 : 0.0;
    const double effective_delay_send = part.delay_send_level() * voice.key_delay_scale();
    const double to_delay = !efx_part && delay && effective_delay_send > 0.0
                                ? SystemDelay::send_gain(effective_delay_send)
                                : 0.0;

    // The voice gain stays a separate multiply from the pan and send levels: collapsing them would
    // be the cheaper kernel and a silently different render, since float multiplication is no more
    // associative than addition is.
    //
    // A part with the EQ on lands in the EQ buffers instead of the output, and is filtered and
    // summed in later. Only the dry path moves; the sends below are fed the same either way.
    std::span<float> dry_left = efx_part          ? std::span<float>{efx_in_left}
                                : part.eq_enabled ? std::span<float>{eq_left}
                                                  : left;
    std::span<float> dry_right = efx_part          ? std::span<float>{efx_in_right}
                                 : part.eq_enabled ? std::span<float>{eq_right}
                                                   : right;
    simd::mix_scaled_varying(block, gains, pan_left, dry_left);
    simd::mix_scaled_varying(block, gains, pan_right, dry_right);

    // A send at zero is skipped rather than multiplied out, which drops three of the five passes
    // over the block on a part with no sends. That is exact rather than merely harmless: the buses
    // are cleared to +0 at the top of every block, and a sum that starts at +0 can never reach -0,
    // so adding the +-0 the multiply would have produced leaves every element as it stands.
    if (to_reverb != 0.0) {
        simd::mix_scaled_varying(block, gains, to_reverb, reverb_bus);
    }
    if (to_chorus != 0.0) {
        simd::mix_scaled_varying(block, gains, to_chorus, chorus_bus);
    }
    if (to_delay != 0.0) {
        simd::mix_scaled_varying(block, gains, to_delay, delay_bus);
    }
}

void ToneGenerator::Impl::mix_effects(std::span<float> left, std::span<float> right)
{
    ensure_effects();

    // `scale` is the network's wet level, which is not part of its coefficients: it scales what
    // leaves the network rather than shaping it. Unity is skipped rather than multiplied out, so a
    // stream that never edits a level renders exactly as it did before levels existed.
    const auto add = [&](MatrixRamp* ramp, int level) {
        // No level to track means the network goes in at unity, and unity is skipped rather than
        // multiplied out, so a stream that never edits a level renders exactly as it did before
        // levels existed.
        if (ramp == nullptr) {
            simd::add(wet_left, left);
            simd::add(wet_right, right);
            return;
        }

        // A level already settled at unity is skipped rather than multiplied out, which keeps the
        // old promise: a stream that never edits a level renders bit for bit as it did before any
        // of this existed. Tested *before* the fill, because a coefficient that merely arrives at
        // unity during this block spent the earlier samples somewhere else.
        const bool settled_at_unity = ramp->current() == MatrixRamp::target_of(unity_level)
                                      && level == unity_level;

        // Otherwise a level moves as a *ramp*, not a step: `fx_process_block` walks its matrix
        // coefficient sixteen times a block toward the new value, so a level edit takes about 25 ms
        // to land. Applying it instantly is a click on every change.
        ramp->fill(level, level_gains);
        if (settled_at_unity) {
            simd::add(wet_left, left);
            simd::add(wet_right, right);
            return;
        }
        for (int n = 0; n < block_size; ++n) {
            const auto i = static_cast<std::size_t>(n);
            left[i] += static_cast<float>(static_cast<double>(wet_left[i]) * level_gains[i]);
            right[i] +=
                static_cast<float>(static_cast<double>(wet_right[i]) * level_gains[i]);
        }
    };

    // The same order the module mixes in: chorus, delay, then reverb. The order is not cosmetic --
    // it is what lets the chorus feed the delay and both feed the reverb inside one block.
    chorus_mono.fill(0.0F);
    delay_mono.fill(0.0F);
    chorus_to_reverb.fill(0.0F);
    chorus_to_delay.fill(0.0F);
    delay_to_reverb.fill(0.0F);

    // Each send is a ramped coefficient of the same matrix the levels sit in, so it is applied
    // here rather than inside a network: a network compiles its coefficients once, and a step where
    // the module glides is a click. `MatrixRamp::send_shift` is the only thing that differs.
    const auto scale_feed =
        [&](MatrixRamp& ramp, int level, const std::array<float, block_size>& from,
            std::array<float, block_size>& into) {
            ramp.fill(level, send_gains, MatrixRamp::send_shift);
            for (int n = 0; n < block_size; ++n) {
                const auto i = static_cast<std::size_t>(n);
                into[i] = static_cast<float>(static_cast<double>(from[i]) * send_gains[i]);
            }
        };

    if (chorus) {
        // The send buses as the effects receive them, for comparison against the module's own.
        //
        // `scdec chorusin` reads the module's at 0x181a19070 (chorus) and 0x181a190f0 (delay) --
        // the arguments `fx_process_block` loads into rdx before each stage. Those had been taken
        // for the left and right of one stereo chorus, which is why the chorus input never appeared
        // to respond to CC#93: every earlier read was of the delay's bus. Printing this port's at
        // the same point makes the two directly comparable, which is what the panwet chorus deficit
        // has been missing -- every coefficient on the path already matches.
        //
        // The deficit is **2.95x**, measured by rendering the file three ways through each engine
        // and differencing -- normal, CC#93 zeroed, both sends zeroed -- so the dry that fed the
        // effect cancels instead of being counted as wet. The 5.79 an earlier note carried is that
        // figure multiplied by the per-part send clamp this port does not yet apply: a projection
        // of where the deficit would land if the clamp were fixed on its own, which is exactly why
        // it must not be. The same pass put the reverb return at 0.99, so this is the chorus
        // return alone and not the send network.
        static const bool probe_buses = std::getenv("TS_SEND_BUS_PROBE") != nullptr;
        if (probe_buses) {
            const auto stats = [](std::span<const float> bus) {
                double peak = 0.0;
                double sum = 0.0;
                for (const float v : bus) {
                    peak = std::max(peak, std::abs(static_cast<double>(v)));
                    sum += static_cast<double>(v) * static_cast<double>(v);
                }
                return std::pair{peak, std::sqrt(sum / static_cast<double>(bus.size()))};
            };
            const auto [cp, cr] = stats(chorus_bus);
            const auto [dp, dr] = stats(delay_bus);
            std::fprintf(stderr, "sendbus: chorus peak=%.9f rms=%.9f  delay peak=%.9f rms=%.9f\n",
                         cp, cr, dp, dr);
        }
        // The chorus's own contribution, taken by differencing the wet buses across its call,
        // because it accumulates into shared ones. Comparable to the module's two output taps at
        // 0x181a19170 and 0x181a191f0 -- fx_chorus_stage_l writes param_3 and param_3+0x80, so it
        // is mono in and stereo out.
        std::array<float, block_size> wet_l_before{};
        std::array<float, block_size> wet_r_before{};
        if (probe_buses) {
            std::copy(wet_left.begin(), wet_left.end(), wet_l_before.begin());
            std::copy(wet_right.begin(), wet_right.end(), wet_r_before.begin());
        }
        chorus->process(chorus_bus, wet_left, wet_right, chorus_mono);
        if (probe_buses) {
            double pl = 0.0;
            double sl = 0.0;
            double pr = 0.0;
            double sr = 0.0;
            for (int n = 0; n < block_size; ++n) {
                const auto i = static_cast<std::size_t>(n);
                const double dl =
                    static_cast<double>(wet_left[i]) - static_cast<double>(wet_l_before[i]);
                const double dr =
                    static_cast<double>(wet_right[i]) - static_cast<double>(wet_r_before[i]);
                pl = std::max(pl, std::abs(dl));
                sl += dl * dl;
                pr = std::max(pr, std::abs(dr));
                sr += dr * dr;
            }
            std::fprintf(stderr, "chorusout: L peak=%.9f rms=%.9f  R peak=%.9f rms=%.9f\n",
                         pl, std::sqrt(sl / block_size), pr, std::sqrt(sr / block_size));
        }
        add(&chorus_level_ramp, chorus_level);
        scale_feed(chorus_to_reverb_ramp, chorus_to_reverb_level, chorus_mono, chorus_to_reverb);
        scale_feed(chorus_to_delay_ramp, chorus_to_delay_level, chorus_mono, chorus_to_delay);
    }
    if (delay) {
        // The chorus's send-to-delay lands on the delay's own bus, ahead of its input conditioner,
        // exactly where a part's delay send lands.
        simd::add(chorus_to_delay, delay_bus);
        delay->process(delay_bus, wet_left, wet_right, delay_mono);
        add(&delay_level_ramp, delay_level);
        scale_feed(delay_to_reverb_ramp, delay_to_reverb_level, delay_mono, delay_to_reverb);
    }
    if (reverb) {
        reverb->process(reverb_bus, chorus_to_reverb, delay_to_reverb, wet_left, wet_right);
        add(&reverb_level_ramp, reverb_level);
    }
}

void ToneGenerator::Impl::ensure_effects()
{
    // A type change mid-stream replaces the network, which clears its tail. The offline renderer
    // cannot do this at all -- it picks the last type the file selects and runs the whole song
    // through it -- so a file that switches types part way sounds different here, and closer to the
    // module.
    // An edited row is rebuilt from the row itself rather than from the macro, which is the whole
    // point of keeping it: `Reverb::for_type` can only produce what the macro produced.
    if (options.reverb && (!reverb || reverb_built_for != reverb_type || reverb_row_edited)) {
        reverb.emplace(reverb_row_edited
                           ? Reverb{EffectProgrammer::reverb_from_row(notes->rom(), reverb_row)}
                           : Reverb::for_type(reverb_type));
        reverb_built_for = reverb_type;
        reverb_row_edited = false;
    }
    if (options.chorus && (!chorus || chorus_built_for != chorus_type || chorus_row_edited)) {
        // **The LFO accumulator survives the rebuild**, because on the module it survives a macro
        // change: watching the register through a GS reset shows the rate replaced from 1024 to
        // 192 while the phase carries straight on, and a reset does not zero it either. Building a
        // fresh `Chorus` here would restart the LFO every time a file selected a chorus type, which
        // most files do once at the top and some do repeatedly.
        const int carried = chorus ? chorus->lfo_phase() : chorus_phase_seed;
        chorus.emplace(chorus_row_edited
                           ? Chorus{EffectProgrammer::chorus_from_row(notes->rom(), chorus_row)}
                           : Chorus::for_type(chorus_type));
        chorus->set_phase(carried);
        chorus_built_for = chorus_type;
        chorus_row_edited = false;
    }
    if (options.delay && (!delay || delay_built_for != delay_type || delay_row_edited)) {
        // Once a row exists it is the authority, exactly as the reverb's and chorus's are. An
        // unedited row compiles to what `for_type` produces -- same function, same ten values --
        // so selecting a macro and changing nothing is not a different network, only a longer way
        // of naming the same one.
        delay.emplace(delay_row_loaded ? SystemDelay{SystemDelay::compile(delay_row)}
                                       : SystemDelay::for_type(delay_type.value_or(0)));
        delay_built_from_row = delay_row_loaded;
        delay_built_for = delay_type;
        delay_row_edited = false;
    }
}

// ---------------------------------------------------------------------------------------------
// Voice lifetime
// ---------------------------------------------------------------------------------------------

void ToneGenerator::Impl::steal(int index)
{
    // The slot is about to be reassigned, so whatever was sounding moves to the dying list and
    // fades. Cutting it dead instead clicks.
    auto& slot = slots[static_cast<std::size_t>(index)];
    if (!slot) {
        return;
    }
    slot->choke();
    dying.push_back(std::move(slot));
}

void ToneGenerator::Impl::release_slot(int index)
{
    auto& slot = slots[static_cast<std::size_t>(index)];
    if (slot) {
        slot->kill();
        spare.push_back(std::move(slot));
    }
}

void ToneGenerator::Impl::recycle(int index)
{
    release_slot(index);
    pool.free_slot(index);
}

int ToneGenerator::Impl::allocate_slot(int channel, int note, int velocity, int group)
{
    // Allocating may steal, which hands whatever was sounding to the dying list and empties the
    // slot; anything still there was already finished.
    const Voice slot = pool.allocate(channel, note, velocity, group);

    // A growing pool may have just made room; the voice storage follows it. This is the allocation
    // that makes the growing mode unsafe on an audio thread, and it happens here rather than
    // hidden inside the pool so it is visible at the point it costs something.
    if (static_cast<int>(slots.size()) < pool.capacity()) {
        slots.resize(static_cast<std::size_t>(pool.capacity()));
    }

    release_slot(slot.index);
    return slot.index;
}

void ToneGenerator::Impl::install(int slot, VoiceSetup&& setup)
{
    // Read here rather than at allocation, because the module reads them here: its setup pass runs
    // after the whole chunk's messages have reached their parts, so a volume or resonance change
    // that arrived after the note-on in the same chunk is already in force by the time the voice
    // takes its value. The fader starts *at* the part's volume rather than gliding up to it --
    // `tvf_env_prep` writes the same value into the ramp's source and target slots.
    const Part& part = parts[static_cast<std::size_t>(setup.channel)];
    setup.volume_word = part.volume_word();
    setup.volume_mask = volume_ramp_mask;
    setup.part_resonance = part.tvf_resonance;

    std::unique_ptr<PartialVoice> voice;
    if (!spare.empty()) {
        voice = std::move(spare.back());
        spare.pop_back();
    } else {
        voice = std::make_unique<PartialVoice>(notes->interpolator(), notes->tvf(), notes->pan());
    }

    voice->start(std::move(setup));
    slots[static_cast<std::size_t>(slot)] = std::move(voice);
}

void ToneGenerator::Impl::start_pending_voices()
{
    if (pending_notes.empty()) {
        return;
    }

    // Stable, so two notes queued for the same part keep the order they arrived in: the module's
    // per-part list preserves that, and only the order *between* parts is the block's business.
    std::stable_sort(pending_notes.begin(), pending_notes.end(),
                     [](const PendingNote& left, const PendingNote& right) {
                         return block_order_key(left.part, Sequence::channel_count)
                                < block_order_key(right.part, Sequence::channel_count);
                     });

    std::vector<PendingNote> starting;
    starting.swap(pending_notes);

    // Two passes per part, not one pass per note, and the difference only shows on a chord.
    // `note_on_voice_setup @ 18005f5c0` calls nothing but `voice_init_fresh`,
    // `voice_init_from_parent` and `prng_lfsr`: it gathers every node waiting on the part and seeds
    // all of them before any partial's parameters are read. Interleaving the two -- seed a note,
    // read it, seed the next -- puts every draw after the first note at the wrong position, which
    // is audible through the random pan and cost two songs their balance row when it was tried.
    //
    // `starting` is sorted by block order and stable, so a run of equal `part` is one batch.
    for (std::size_t begin = 0; begin < starting.size();) {
        std::size_t end = begin;
        while (end < starting.size() && starting[end].part == starting[begin].part) {
            ++end;
        }

        seed_part_lfo_nodes(std::span{starting}.subspan(begin, end - begin));

        for (std::size_t index = begin; index < end; ++index) {
            read_pending_note(starting[index]);
        }
        begin = end;
    }
}

void ToneGenerator::Impl::read_pending_note(PendingNote& note)
{
    {
        if (note.voices.empty()) {
            return;
        }

        // One pan for the whole note, drawn at the end of the first voice's parameter load -- after
        // that voice's own two pitch draws, which is where `tvf_env_prep` sits in
        // `partial_load_params`.
        std::optional<int> random_pan;
        for (PendingVoice& pending : note.voices) {
            const int jitter =
                notes->pitch().base_pitch_jitter_milli_semitones(pending.setup.partial);
            if (pending.setup.is_drum) {
                pending.setup.drum_base_ratio =
                    std::pow(2.0,
                             (std::max(0, pending.drum_pitch_raw + jitter) - pending.native)
                                 / 12000.0);
            } else {
                pending.setup.base_pitch =
                    std::max(0, pending.base_pitch_raw + jitter) + pending.scale_offset;
            }

            pending.setup.pitch_envelope = notes->pitch().create_envelope_runner(
                pending.setup.partial, pending.key, pending.velocity);

            if (note.random_pan && !random_pan) {
                random_pan = notes->noise().next_pan();
            }
            pending.setup.random_pan = random_pan;

            install(pending.slot, std::move(pending.setup));
        }

        // Released before it ever sounded. The release runs on the voice rather than skipping it,
        // because that is what the module's output shows: the note is audible for its first block
        // and then decays, rather than being silent. Held to the end of the note's setup so every
        // partial of a group is released together, and so the release sees a fully started voice.
        if (note.released) {
            for (const PendingVoice& pending : note.voices) {
                if (auto& slot = slots[static_cast<std::size_t>(pending.slot)]) {
                    if (note.choke) {
                        slot->choke();
                    } else {
                        slot->note_off(note.release_damper);
                    }
                }
            }
        }
    }
}

bool ToneGenerator::Impl::any_voice_on(int channel) const
{
    // A note allocated earlier in this chunk counts as sounding even though its slot is still
    // empty: the module hangs the group off the part the moment it allocates, so a second note-on
    // in the same chunk already sees the part as busy.
    for (const PendingNote& queued : pending_notes) {
        if (queued.part == channel && !queued.voices.empty()) {
            return true;
        }
    }
    for (const auto& slot : slots) {
        if (slot && !slot->finished() && slot->channel() == channel) {
            return true;
        }
    }
    return false;
}

void ToneGenerator::Impl::stop_note(int channel, int note, int damper)
{
    bool sounding = false;
    for (auto& slot : slots) {
        if (!slot || slot->channel() != channel || slot->note() != note || slot->finished()) {
            continue;
        }
        // Still ringing counts, even if it is already in release: a voice the re-strike above took
        // is exactly what a stray note-off for that key is *for*. See below.
        sounding = true;
        if (!slot->released()) {
            slot->note_off(damper);
        }
    }

    // Only then the notes allocated earlier in this chunk, whose slots are still empty.
    // `any_voice_on` already counts these as sounding, for the same reason: between allocation and
    // the setup pass the note exists on the part in the module, so anything that asks about the
    // part has to see it. A note-off is one of those things. See `PendingNote::released`.
    //
    // **Older instance first, and the order is the whole of it.** A sequencer writing a repeated
    // key routinely emits the new note-on before the old note's note-off, so at the moment the off
    // arrives there are two instances of the same key on the part, one ringing and one queued, and
    // the off belongs to the older. The queued note is reached only when nothing is left ringing to
    // take the release instead.
    //
    // **Which is why the loop above counts a released voice.** The note-on handler re-strikes by
    // calling this function first, so by the time the paired note-off arrives the older voice has
    // already been released and would look like nothing at all -- and then the off falls through
    // to the note that was struck a moment ago and kills it. `robyn_show_me_love.mid` does exactly
    // this on note 40: struck at 39.130 s, re-struck at 39.616 s with the older note's off
    // immediately behind it, and the new note is meant to ring until 40.085 s. Not counting the
    // ringing voice cut the level to a third for the rest of the phrase, twice in the song.
    //
    // A voice that has finished does not count, which is what keeps this from swallowing a real
    // zero-length note on a key played earlier in the song.
    if (!sounding) {
        for (PendingNote& queued : pending_notes) {
            if (queued.part == channel && queued.note == note && !queued.released) {
                queued.released = true;
                queued.release_damper = damper;
            }
        }
    }

    pool.release(channel, note);
}

ToneGenerator::Impl::Envelopes ToneGenerator::Impl::envelopes(int tone_number,
                                                              const PartialParameters& partial,
                                                              int key,
                                                              int velocity,
                                                              const PartModifiers& modifiers,
                                                              std::optional<int> rate_key)
{
    const int zone_level =
        notes->directory().zone_level(partial.multisample(), key, partial.key_center());

    SegmentEnvelope amplitude =
        notes->tva().create_envelope(partial,
                                     velocity,
                                     key,
                                     zone_level,
                                     notes->directory().tone_level(tone_number),
                                     sample_rate,
                                     0.0,
                                     rate_key,
                                     modifiers);

    TvfChain::Envelope cutoff =
        notes->tvf().create_envelope(partial, velocity, key, sample_rate, modifiers);

    return Envelopes{std::move(amplitude), std::move(cutoff.offsets), cutoff.base_cutoff};
}

void ToneGenerator::Impl::start_note(int channel, int note, int velocity)
{
    if (is_drum_part(channel)) {
        start_drum(channel, note, velocity);
        return;
    }

    Part& part = parts[static_cast<std::size_t>(channel)];

    // GS keyboard range, SysEx-only and defaulted wide open.
    //
    // **Both bounds are read as `int8`, because the engine reads them that way**: the note-on test
    // is `*(char *)(part+0x3e0) <= note && note <= *(char *)(part+0x3e1)`, signed on both sides. A
    // parameter change cannot put anything above 0x7f there -- a DT1 data byte has seven bits -- but
    // a **patch bulk dump can**, since its two nibbles compose a whole byte. A low bound of 0xff is
    // then -1 on the module and passes everything, where reading it unsigned rejects every note.
    //
    // Measured rather than reasoned: `darkness3.mid` sounds 3,607 notes with its dump applied and
    // 3,226 when this comparison is unsigned. The difference is entirely parts whose dumped low
    // bound has the top bit set.
    const int key_low = static_cast<std::int8_t>(part.key_low);
    const int key_high = static_cast<std::int8_t>(part.key_high);
    if (note < key_low || note > key_high) {
        return;
    }

    // Velocity sense, applied here rather than deeper because the engine applies it once at
    // note-on and everything downstream reads the result -- the level chain, both envelope
    // velocity scales and the filter's own velocity term all take the sensed value, not the
    // value that arrived on the wire.
    velocity = part.effective_velocity(velocity);

    // GS part key shift (`40 1x 16`), and it moves the NOTE NUMBER rather than the pitch.
    //
    // The distinction is invisible on most tones and decisive on a few. Transposing the note ahead
    // of the lookup lets it land in a different sample zone; adding the same interval to the pitch
    // afterwards keeps the untransposed note's zone and stretches it. Where a tone's zones are
    // uniformly tuned -- a finely multisampled piano -- the two agree exactly, which is why this
    // stood for so long.
    //
    // Measured on program 77, whose low zone plays an octave up and whose zone above key 71 does
    // not. Note 60, key shift swept, module against this engine before the change:
    //
    //     shift  +0   +2   +4   +6   +8  +10  | +12  +16  +24
    //     module +12  +14  +16  +18  +20  +22 | +12  +16  +24     = pitch of note (60 + shift)
    //     ours   +12  +14  +16  +18  +20  +22 | +24  +28  +36     = note 60's zone, then + shift
    //
    // Both columns agree while the shifted note stays inside the low zone and part company the
    // moment it crosses out of it, which is the signature of a lookup-time transpose rather than a
    // playback-time one. `shangai.mid` is the corpus's one file that shows it: its channel 1 sets
    // shift +12 over this program and sounded an octave high for the whole song.
    //
    // The receive range above is deliberately tested against the arriving note, not this one: it
    // filters what the part accepts, and a transpose is what the part does with what it accepted.
    // Drums never reach here -- `start_drum` returns above -- and the module likewise ignores key
    // shift on a rhythm part, measured the same way.
    const int sounding_note = std::clamp(note + (part.key_shift - 0x40), 0, 0x7F);

    const std::vector<int> tones =
        notes->directory().program_tones(part.program, tone_map_for(part), lookup_bank_for(part));
    if (tones.empty()) {
        return;
    }

    // Mono mode flushes what the part is already sounding, which is also what lets CC#65 portamento
    // engage -- see the gate below.
    if (part.mono) {
        for (auto& slot : slots) {
            if (slot && !slot->finished() && slot->channel() == channel) {
                slot->choke();
            }
        }
    }

    // Resolved at dispatch, because allocation happens here even though nothing is read yet: the
    // count of voices the note takes is what `seed_part_lfo_nodes` later seeds from, and
    // it comes from the queued list rather than being carried separately.
    std::vector<ResolvedTone> layers;
    layers.reserve(tones.size());
    for (int tone_number : tones) {
        if (!notes->directory().tone(tone_number)) {
            layers.emplace_back();
            continue;
        }
        layers.push_back(notes->directory().resolve(tone_number, sounding_note, velocity));
    }
    // A part panpot of zero is GS RND: the engine repositions the note outright rather than
    // offsetting the partial's own pan, and redraws for every note. Only the SysEx panpot can set
    // it -- CC#10 clamps zero to one, so the wheel cannot reach this.
    //
    // **Drawn inside the first partial's setup, not here.** The pan is resolved by the same
    // per-voice pass that computes the pitch (`tvf_env_prep @ 180060620`, at the end of
    // `partial_load_params`), so the first partial's own two pitch draws come first and only then
    // the pan. Drawing it up front placed it one or two values early on every randomly-panned note.
    PendingNote queued;
    queued.part = channel;
    queued.note = note;
    queued.random_pan = part.pan == 0;

    // Portamento. CC#84 names the source key outright, is consumed by this one note, and glides
    // whatever else the part is doing. CC#65 is the sustained mode, and the engine only arms it
    // when the part has nothing left sounding -- measured against the DLL, a note struck over a
    // still-ringing one does not glide, while the same pair in mono mode does.
    //
    // Mono short-circuits the check rather than re-reading it after the flush, exactly as the
    // engine does: the voices it just chased away are still fading, so a fresh count would say the
    // part is busy.
    const bool quiet = part.mono || !any_voice_on(channel);
    const int glide_from = part.portamento_control_key >= 0 ? part.portamento_control_key
                           : (part.portamento_on && quiet)  ? part.last_key
                                                            : -1;
    const int glide_step = notes->pitch().portamento_step(part.portamento_time);
    part.portamento_control_key = -1;
    part.last_key = std::clamp(note, 0, 0x7F);

    const int group = pool.begin_note_group();
    bool sounded = false;

    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        const int tone_number = tones[layer];
        const ResolvedTone& resolved = layers[layer];
        const std::optional<Tone> tone = notes->directory().tone(tone_number);
        if (!tone) {
            continue;
        }

        // One LFO1 node for the whole layer, claimed before its partials are walked, exactly as
        // `partial_alloc_node` does. Its timing is all tone-header -- waveform, phase, rate, delay,
        // fade -- so any partial of the tone configures it identically; only the three depths are
        // the partial's, and those travel beside the node rather than in it.
        std::shared_ptr<LfoRunner> shared_lfo1;

        for (const ResolvedPartial& sounding : resolved.partials) {
            const PartialParameters& partial =
                tone->partials()[static_cast<std::size_t>(sounding.partial_index)];
            const DecodedWave* wave = notes->sampler().decode(sounding.descriptor);
            if (wave == nullptr) {
                continue;
            }

            const int key = sounding_note;
            Envelopes built = envelopes(tone_number, partial, key, velocity, part.modifiers());
            auto [lfo1_config, lfo2_config] =
                notes->lfo().configure(tone_number, partial, part.modifiers());
            if (!shared_lfo1) {
                shared_lfo1 = std::make_shared<LfoRunner>(notes->lfo(), lfo1_config);
            }
            LfoRunner lfo2 = notes->lfo().create_runner(lfo2_config);

            // The base pitch's own random offset, which the module clamps at zero before anything
            // else is folded in -- `partial_compute_pitch` again, and the first of the two draws a
            // voice can make. Scale tuning folds in after it rather than riding with the bend: it
            // is per-key, so it is latched at note-on like the rest of the note's pitch.
            const int base_pitch_raw =
                notes->pitch().base_pitch_milli_semitones(partial, sounding_note,
                                                          partial.key_center());

            VoiceSetup setup;
            setup.channel = channel;
            setup.note = note;
            setup.wave = wave;
            setup.partial = partial;
            setup.descriptor = sounding.descriptor;
            setup.amplitude = std::move(built.amplitude);
            setup.cutoff = std::move(built.cutoff);
            setup.cutoff_base = built.cutoff_base;

            // Where the pan is actually drawn: the end of the first voice's parameter load, after
            // that voice's own two pitch draws. `pitch_env_rand_init @ 180060560` writes the
            // position onto every voice of the group and latches it, so the rest of the note reads
            // it rather than drawing again.
            setup.lfo1 = shared_lfo1;
            setup.lfo1_depths = lfo1_config;
            setup.lfo2 = std::make_shared<LfoRunner>(std::move(lfo2));
            setup.envelope_hold_samples = notes->envelopes().hold_samples(
                partial, velocity, part.envelope_delay + part.envelope_delay_tone);
            setup.half_damper = notes->directory().half_damper(tone_number);
            setup.glide_milli_semitones =
                glide_from < 0
                    ? 0.0
                    : PitchChain::portamento_offset(glide_from, base_pitch_raw);
            setup.glide_step = glide_step;
            setup.extended_interpolation = options.extended_interpolation;
            setup.pan = partial.pan();
            setup.pan_follows_part = true;
            setup.level_gain = 1.0;

            PendingVoice pending;
            pending.slot = allocate_slot(channel, note, velocity, group);
            pending.key = key;
            pending.velocity = velocity;
            pending.base_pitch_raw = base_pitch_raw;
            pending.scale_offset = part.scale_offset_milli_semitones(key);
            pending.setup = std::move(setup);
            queued.voices.push_back(std::move(pending));
            sounded = true;
        }
    }

    if (sounded) {
        pending_notes.push_back(std::move(queued));
        ++note_count;
    }
}

void ToneGenerator::Impl::start_drum(int channel, int note, int velocity)
{
    Part& part = parts[static_cast<std::size_t>(channel)];

    // Velocity sense applies to a drum part too: the engine computes it in the note-on handler both
    // kinds of part share, before either branch runs.
    velocity = part.effective_velocity(velocity);

    // The kit's own key is kept alongside the overridden one: `apply` also resolves the panpot,
    // and the envelope rate key-follow takes the plane through its own clamp.
    DrumKey kit_key = notes->drums().key(note, drum_kit[kit_slot(channel)]);
    const DrumKeyOverrides& planes = drum_setup[kit_slot(channel)];

    // The drum-setup SysEx planes replace their kit entries outright, before the relative NRPN
    // overrides go on top.
    if (const std::optional<int> play_key = planes.play_key(note)) {
        kit_key.pitch = *play_key;
    }
    if (const std::optional<int> level = planes.level(note)) {
        kit_key.level = *level;
    }
    // `kit_` prefixes because the bare names are the effect units themselves, one scope out.
    if (const std::optional<int> kit_reverb = planes.reverb(note)) {
        kit_key.reverb = *kit_reverb;
    }
    if (const std::optional<int> kit_chorus = planes.chorus(note)) {
        kit_key.chorus = *kit_chorus;
    }
    if (const std::optional<int> kit_delay = planes.delay(note)) {
        kit_key.delay = *kit_delay;
    }
    if (const std::optional<int> group = planes.group(note)) {
        kit_key.group = *group;
    }
    if (const std::optional<bool> rx_off = planes.rx_note_off(note)) {
        kit_key.receives_note_off = *rx_off;
    }
    if (const std::optional<bool> rx_on = planes.rx_note_on(note)) {
        kit_key.receives_note_on = *rx_on;
    }

    // A key switched off simply does not sound. The module refuses it at the top of its note-on
    // dispatch (`0x480[key] & 0x10`), before velocity, mute groups or anything else runs, so
    // nothing below this line -- not even the choke -- happens for it. Every kit record ships
    // with every sounding key receiving, so only an explicit drum-setup write can get here.
    if (!kit_key.receives_note_on) {
        return;
    }

    // The key's panpot is resolved without drawing. A zero here means random, but it is the *pan
    // setup* that decides that and draws for it, once, alongside the part's own panpot -- see
    // below. Drawing here as well spent two values on a hit the module spends one on.
    const DrumKey key = DrumKeyOverrides::apply(kit_key,
                                                planes.pitch_offset(note),
                                                planes.pan(note));
    const int rate_key =
        NoteRenderer::envelope_rate_key(kit_key, planes.pitch_offset(note));

    const ResolvedTone resolved = notes->directory().resolve(key.tone, /*note=*/60, velocity);
    const std::optional<Tone> tone = notes->directory().tone(key.tone);
    if (!tone || resolved.partials.empty()) {
        return;
    }

    // A drum in a mute group cuts the last one that sounded in it -- what silences an open hi-hat
    // when the closed one is played.
    if (key.group != 0) {
        for (auto& slot : slots) {
            if (slot && slot->mute_group() == key.group) {
                slot->choke();
            }
        }
    }

    const double level_gain = DrumKitTable::level_gain(key.level);

    // A part panpot of zero is GS RND on a drum part too, and it wins over the kit plane outright:
    // the engine draws and returns before it ever folds the offset in, so eight identical hits
    // land at eight unrelated positions. Only the SysEx panpot reaches it -- CC#10 clamps zero to
    // one -- and the draw is once per note, shared by every partial of it.
    //
    // **A zero on the kit plane means the same thing**, which is the path STREETS.MID takes: it
    // writes drum panpot 0 by NRPN on four cymbals and leaves the part's own panpot centred. The
    // pan setup reads `part[0x3dd]` first and the plane byte `kit[0x280 + key]` second, and either
    // being zero sends it to the same draw -- one draw, not one for each.
    PendingNote queued;
    queued.part = channel;
    queued.note = note;
    queued.random_pan = part.pan == 0 || key.pan == 0;

    const int group = pool.begin_note_group();
    bool sounded = false;

    // The kit's tone gets one LFO1 node for its partials, like any other.
    std::shared_ptr<LfoRunner> shared_lfo1;

    for (const ResolvedPartial& sounding : resolved.partials) {
        const PartialParameters& partial =
            tone->partials()[static_cast<std::size_t>(sounding.partial_index)];
        const DecodedWave* wave = notes->sampler().decode(sounding.descriptor);
        if (wave == nullptr) {
            continue;
        }

        // A drum part carries the same modify offsets as a melodic one: the engine reads them off
        // the part, and nothing in `tva_compute_env_rates` asks whether the voice is a drum.
        Envelopes built = envelopes(key.tone, partial, 60, velocity, part.modifiers(), rate_key);
        auto [lfo1_config, lfo2_config] =
            notes->lfo().configure(key.tone, partial, part.modifiers());
        if (!shared_lfo1) {
            shared_lfo1 = std::make_shared<LfoRunner>(notes->lfo(), lfo1_config);
        }
        LfoRunner lfo2 = notes->lfo().create_runner(lfo2_config);

        // The note does not transpose the sample: the kit's coarse-pitch plane supplies the key,
        // and the tone's own key-follow decides what a step of it is worth.
        const double native = sounding.descriptor.native_milli_semitones();

        // `partial_compute_pitch` does not ask whether a voice is a drum, so a percussion partial
        // with a non-zero +0x12 jitters and draws exactly as a melodic one does.
        //
        // **This was disabled once, on an argument that did not hold.** Adding it moved
        // `canyon.mid` from bit-exact to 1,183,722 samples differing by up to 2,354 LSB on the
        // block-loop gate, which was read as the module making no draw here. That gate is not the
        // module: `fixtures/stream_renders.json` is generated from *this engine* as a regression
        // baseline -- its own note says it "cannot catch a mistake both sides share" -- so all it
        // reported was drift from the engine's previous output, which is what an intended change
        // looks like.
        //
        // Re-measured against the gates that do have an outside opinion, with the jitter on and
        // off: the DLL-derived note gate is unchanged, 3537 assertions and the same three
        // pitch-cents cases either way, and the song oracle gains one -- `dreaming_i_was_dreaming`
        // map 4's peak passes with the jitter and fails without it. Nothing regresses. So the DLL
        // does not contradict what the decompilation plainly says, and the decompilation decides.

        VoiceSetup setup;
        setup.channel = channel;
        setup.note = note;
        setup.wave = wave;
        setup.partial = partial;
        setup.descriptor = sounding.descriptor;
        setup.amplitude = std::move(built.amplitude);
        setup.cutoff = std::move(built.cutoff);
        setup.cutoff_base = built.cutoff_base;
        setup.lfo1 = shared_lfo1;
        setup.lfo1_depths = lfo1_config;
        setup.lfo2 = std::make_shared<LfoRunner>(std::move(lfo2));
        setup.envelope_hold_samples = notes->envelopes().hold_samples(
            partial, velocity, part.envelope_delay + part.envelope_delay_tone);
        setup.is_drum = true;
        // The kit's pan plane is an OFFSET from centre, not an absolute position: the engine bases
        // a drum voice on the part panpot exactly as it does a melodic one and folds the plane in
        // on top (`pan = clamp(part[0x3dd] + (kit[0x280 + key] - 0x40))`, the pan setup at
        // 180060620). CC#10 therefore sweeps a drum part -- SOMDesert-SC8850.mid pans a lone hand
        // clap across the field with nothing else, and treating the plane as absolute pinned it.
        setup.pan = key.pan;
        setup.pan_follows_part = true;
        setup.level_gain = level_gain;
        setup.mute_group = key.group;
        setup.drum_receives_note_off = key.receives_note_off;
        setup.key_reverb = key.reverb;
        setup.key_chorus = key.chorus;
        setup.key_delay = key.delay;

        PendingVoice pending;
        pending.slot = allocate_slot(channel, note, velocity, group);
        pending.key = 60;
        pending.velocity = velocity;
        pending.drum_pitch_raw = PitchChain::drum_pitch_milli_semitones(partial, key.pitch);
        pending.native = native;
        pending.setup = std::move(setup);
        queued.voices.push_back(std::move(pending));
        sounded = true;
    }

    if (sounded) {
        pending_notes.push_back(std::move(queued));
        ++note_count;
    }
}

void ToneGenerator::Impl::seed_part_lfo_nodes(std::span<PendingNote> batch)
{
    // What `note_on_voice_setup @ 18005f5c0` takes from the shared generator before a single one of
    // the part's parameters is read, and **it is not a discard**. Every LFO node it initialises gets
    // `+0x7a = prng_lfsr()`, unconditionally -- that register is the random shapes' held value, so a
    // sample-and-hold opens on a real draw rather than at zero.
    //
    // **One batch per part per dispatch chunk, not one per note.** Measured with `scdec lfotrace`,
    // which reads the module's own nodes rather than inferring from audio, on `Bubble` -- two
    // partials and a random LFO1, so three nodes and three draws. The generator's sequence from the
    // 0xEFA6/0x9C23 seeds opens 20373, 19301, 31980, -29494, 8988, ...
    //
    //   one note                    LFO1 seeds on draw 1, wraps resume at 4
    //   two notes, two channels     LFO1 seeds on draws 1 and 4 -- each part pays
    //   two notes, one channel      both LFO1 nodes seed on draw 1, wraps resume at 4
    //
    // So a second note arriving on a part in the same chunk pays nothing: its nodes come up already
    // carrying the first note's values. That is the observation. The path is presumably
    // `voice_init_from_parent`, which `note_on_voice_setup` calls before its node loop, but the
    // linkage was not traced -- what is measured is that no second draw happens.
    //
    // **An older probe had both halves of this right and they were being conflated.** It measured
    // "two notes on the same tick cost the same as one", which is true *on one part*, and "a second
    // note elsewhere costs `partials + 1`", which is true *across parts*. Taking the first as the
    // general rule kept a blind discard here; taking the second as the general rule made a chord
    // draw too much and cost `roland_sc88_y03` and `roland_suplex` their balance rows.
    //
    // The pan is the exception and stays per note -- see `read_pending_note`. With the part's panpot
    // at a literal zero, one note's wraps resume at 5 instead of 4 and a two-note chord's at 6, so
    // each note draws its own position even when the nodes were only seeded once.
    //
    // Order within the batch is the claim order in `partial_alloc_node`: the shared LFO1 first, then
    // each partial's LFO2.
    PendingNote* first = nullptr;
    for (PendingNote& note : batch) {
        if (note.voices.empty()) {
            continue;
        }

        if (first == nullptr) {
            first = &note;
            if (const std::shared_ptr<LfoRunner>& shared = note.voices.front().setup.lfo1) {
                shared->seed_random();
            }
            for (PendingVoice& pending : note.voices) {
                if (pending.setup.lfo2) {
                    pending.setup.lfo2->seed_random();
                }
            }
            take_standing_nodes(note);
            continue;
        }

        // A node with bit 5 set inherits from this part's standing node instead, and costs no
        // draw. Checked before the batch rule, because it is the module's own rule and the batch
        // one is this port's approximation of what a same-tick chord does.
        if (adopt_standing_nodes(note)) {
            take_standing_nodes(note);
            continue;
        }

        const std::shared_ptr<LfoRunner>& source = first->voices.front().setup.lfo1;
        const std::shared_ptr<LfoRunner>& shared = note.voices.front().setup.lfo1;
        if (source && shared && shared != source) {
            shared->copy_random_hold(*source);
        }

        // Positional, because the module's nodes are claimed in partial order. A layered note with
        // more voices than the one that seeded simply has nothing to copy for the extras.
        const std::size_t common = std::min(note.voices.size(), first->voices.size());
        for (std::size_t index = 0; index < common; ++index) {
            if (note.voices[index].setup.lfo2 && first->voices[index].setup.lfo2) {
                note.voices[index].setup.lfo2->copy_random_hold(
                    *first->voices[index].setup.lfo2);
            }
        }
        take_standing_nodes(note);
    }
}

bool ToneGenerator::Impl::adopt_standing_nodes(PendingNote& note)
{
    StandingLfoNodes& standing = standing_lfo[static_cast<std::size_t>(note.part)];
    bool adopted = false;

    const std::shared_ptr<LfoRunner>& shared = note.voices.front().setup.lfo1;
    if (shared && shared->config().shares_node && standing.lfo1 && standing.lfo1 != shared) {
        shared->adopt(*standing.lfo1);
        adopted = true;
    }
    for (std::size_t index = 0; index < note.voices.size() && index < standing.lfo2.size();
         ++index) {
        const std::shared_ptr<LfoRunner>& lfo2 = note.voices[index].setup.lfo2;
        const std::shared_ptr<LfoRunner>& stood = standing.lfo2[index];
        if (lfo2 && lfo2->config().shares_node && stood && stood != lfo2) {
            lfo2->adopt(*stood);
            adopted = true;
        }
    }
    return adopted;
}

void ToneGenerator::Impl::take_standing_nodes(PendingNote& note)
{
    StandingLfoNodes& standing = standing_lfo[static_cast<std::size_t>(note.part)];

    const std::shared_ptr<LfoRunner>& shared = note.voices.front().setup.lfo1;
    if (shared && shared->config().shares_node) {
        standing.lfo1 = shared;
    }
    for (std::size_t index = 0; index < note.voices.size() && index < standing.lfo2.size();
         ++index) {
        const std::shared_ptr<LfoRunner>& lfo2 = note.voices[index].setup.lfo2;
        if (lfo2 && lfo2->config().shares_node) {
            standing.lfo2[index] = lfo2;
        }
    }
}

void ToneGenerator::Impl::release_sustained(int channel, Part& part)
{
    // Oldest first, which is the order the offline renderer closes them in too. The damper value
    // at the lift reaches the release rate on half-damper tones, so a pedal eased up through
    // 1-0x3f gives those notes a proportionally longer tail.
    const std::vector<int> releasing = part.sustained;
    part.sustained.clear();
    for (int note : releasing) {
        stop_note(channel, note, part.damper);
    }
}

void ToneGenerator::Impl::flush_part_voices(int channel)
{
    for (auto& slot : slots) {
        if (slot && !slot->finished() && slot->channel() == channel) {
            slot->choke();
        }
    }
}

/// Applies a decoded parameter to a live part, through the Rx gate that guards it.
///
/// The decode is shared with the offline path; this is not, and should not be. A running part has
/// receive switches and level setters that a timeline has no notion of, so what a message *means*
/// is common and what it *does* is each front end's own. Returns false for a target this function
/// does not own, which leaves the caller's own handling to run.
[[nodiscard]] bool apply_control_update(Part& part, const ControlUpdate& update)
{
    const int value = update.value;
    switch (update.target) {
    case ControlTarget::bank:
        if (part.rx.bank_msb) {
            part.bank = value;
        }
        return true;
    case ControlTarget::modulation:
        if (part.rx.modulation) {
            part.modulation = value;
        }
        return true;
    case ControlTarget::volume:
        if (part.rx.volume) {
            part.set_volume(value);
        }
        return true;
    case ControlTarget::pan:
        if (part.rx.panpot) {
            part.pan = value;
        }
        return true;
    case ControlTarget::expression:
        if (part.rx.expression) {
            part.set_expression(value);
        }
        return true;
    case ControlTarget::reverb_send:
        part.reverb_send = value;
        return true;
    case ControlTarget::chorus_send:
        part.chorus_send = value;
        return true;
    case ControlTarget::delay_send:
        part.delay_send = value;
        return true;
    case ControlTarget::bend_range:
        part.bend_range = std::clamp(value, 0, 24);
        return true;

    case ControlTarget::vibrato_rate:
        part.vibrato_rate = value;
        return true;
    case ControlTarget::vibrato_depth:
        part.vibrato_depth = value;
        return true;
    case ControlTarget::vibrato_delay:
        part.vibrato_delay = value;
        return true;
    case ControlTarget::tvf_cutoff:
        part.tvf_cutoff = value;
        return true;
    case ControlTarget::tvf_resonance:
        part.tvf_resonance = value;
        return true;
    case ControlTarget::env_attack:
        part.env_attack = value;
        return true;
    case ControlTarget::env_decay:
        part.env_decay = value;
        return true;
    case ControlTarget::env_release:
        part.env_release = value;
        return true;

    case ControlTarget::velocity_depth:
        part.velocity_depth = value;
        return true;
    case ControlTarget::velocity_offset:
        part.velocity_offset = value;
        return true;
    case ControlTarget::channel_pressure:
        part.channel_pressure = value;
        return true;

    case ControlTarget::matrix_modulation_pitch:
        part.control.at(ControlMatrix::Source::modulation, ControlMatrix::Destination::pitch) =
            value;
        return true;
    case ControlTarget::matrix_pressure_pitch:
        part.control.at(ControlMatrix::Source::channel_pressure,
                        ControlMatrix::Destination::pitch) = value;
        return true;

    case ControlTarget::eq_enabled:
        part.eq_enabled = value != 0;
        return true;

    // Damper's release behaviour and the global EQ block are the caller's.
    case ControlTarget::damper:
    case ControlTarget::eq_low_frequency:
    case ControlTarget::eq_low_gain:
    case ControlTarget::eq_high_frequency:
    case ControlTarget::eq_high_gain:
        return false;
    }
    return false;
}

void ToneGenerator::Impl::control_change(int channel, Part& part, int controller, int value)
{
    // Every controller handler in the engine opens with a test of the part's Rx-CC gate; only the
    // channel-mode messages (CC#120 up) bypass it.
    if (controller < 120 && !part.rx.control_change) {
        return;
    }

    // The two assignable matrix sources, before anything else and without an early return: a
    // controller feeding one of them keeps whatever other meaning it has, because the number is a
    // pointer to a message rather than a claim on it. Pointing CC1 at the mod wheel gives a part
    // whose wheel drives both its own routes and CC1's.
    //
    // The default numbers, 16 and 17, are General Purpose 1 and 2 -- controllers nothing else in
    // this engine reads. That is what they are for.
    if (controller == part.cc1_number) {
        part.cc1 = value;
    }
    if (controller == part.cc2_number) {
        part.cc2 = value;
    }

    // XG swaps what the bank pair means, so it is taken before the shared decoder rather than
    // after: the MSB chooses melodic against drums and the LSB carries the variation, which is the
    // GS reading with the two exchanged. Everything else on an XG part is an ordinary controller.
    if (xg_mode && (controller == 0 || controller == 32)) {
        if (controller == 0) {
            if (part.rx.bank_msb) {
                part.xg_bank_msb = value;
            }
        } else if (part.rx.bank_lsb) {
            part.bank = value;
        }
        return;
    }

    // What the controller means is decided once, in the decoder both front ends share; what it
    // does to a live part is decided here.
    if (const std::optional<ControlUpdate> update =
            decode_control_change(channel, controller, value)) {
        if (apply_control_update(part, *update)) {
            return;
        }
    }

    switch (controller) {
    // Bank select LSB is the tone-map select on this module: 1-4 name a vintage, 0 keeps the
    // default. Stored raw here and interpreted at resolution time (`tone_map_for`).
    case 32:
        if (part.rx.bank_lsb) {
            part.bank_lsb = value;
        }
        break;
    // CC#10 zero is stored as one, so the wheel cannot reach the random position: only the GS
    // SysEx panpot writes a true zero, which is what RND is.
    case 10:
        if (part.rx.panpot) {
            part.pan = value == 0 ? 1 : value;
        }
        break;
    case 11:
        if (part.rx.expression) {
            part.set_expression(value);
        }
        break;
    case 91:
        part.reverb_send = value;
        break;
    case 93:
        part.chorus_send = value;
        break;
    // CC#94 is the delay send's Control Change alias: the engine routes it into the same part
    // byte the `40 1x 2C` SysEx writes (`caseD_5e`).
    case 94:
        part.delay_send = value;
        break;

    case 64:
        if (!part.rx.hold) {
            break;
        }
        part.damper = value;
        if (value < 0x40) {
            release_sustained(channel, part);
        }
        break;

    case 5:
        if (part.rx.portamento) {
            part.portamento_time = value;
        }
        break;
    case 65:
        if (part.rx.portamento) {
            part.portamento_on = value >= 0x40;
        }
        break;
    case 84:
        part.portamento_control_key = value;
        break;

    // CC#67 soft pedal, binary like sostenuto -- the engine reads only bit 6.
    case 67:
        if (part.rx.soft) {
            part.soft = value >= 0x40;
        }
        break;

    case 66:
        if (!part.rx.sostenuto) {
            break;
        }
        // Binary -- the engine reads only bit 6, so there is no half-sostenuto.
        if (value >= 0x40) {
            if (!part.sostenuto_down) {
                part.sostenuto_down = true;
                for (const auto& slot : slots) {
                    if (slot && !slot->finished() && !slot->released() && slot->channel() == channel
                        && std::find(part.sostenuto_captured.begin(),
                                     part.sostenuto_captured.end(),
                                     slot->note())
                               == part.sostenuto_captured.end()) {
                        part.sostenuto_captured.push_back(slot->note());
                    }
                }
            }
        } else if (part.sostenuto_down) {
            part.sostenuto_down = false;
            const std::vector<int> deferred = part.sostenuto_released;
            for (int note : deferred) {
                // The deferred release engages through the standard machinery, so it composes: a
                // still-down damper keeps holding the note, and a part-lifted one scales the
                // release on the half-damper tones.
                if (part.damper_down()) {
                    part.sustained.push_back(note);
                } else {
                    stop_note(channel, note, part.damper);
                }
            }
            part.sostenuto_captured.clear();
            part.sostenuto_released.clear();
        }
        break;

    case 101:
        part.rpn_msb = value;
        part.data_entry_is_nrpn = false;
        break;
    case 100:
        part.rpn_lsb = value;
        part.data_entry_is_nrpn = false;
        break;
    case 99:
        part.nrpn_msb = value;
        part.data_entry_is_nrpn = true;
        break;
    case 98:
        part.nrpn_lsb = value;
        part.data_entry_is_nrpn = true;
        break;

    case 6:
        commit_data_entry_msb(channel, part, value);
        break;
    case 38:
        commit_data_entry_lsb(part, value);
        break;

    case 120:
    case 123:
        // Both take a zero data byte only, as the engine's `caseD_78`/`caseD_7b` do.
        if (value != 0) {
            break;
        }
        for (auto& slot : slots) {
            if (slot && slot->channel() == channel) {
                // All Sound Off cuts; All Notes Off releases. Both stop the note sounding, and the
                // fade is what keeps the cut from clicking.
                if (controller == 120) {
                    slot->choke();
                } else {
                    slot->note_off();
                }
            }
        }
        // Notes allocated earlier in this chunk have no slot to reach yet; both messages have to
        // catch them there instead, for the reason `PendingNote::released` gives. Untested against
        // the module -- no corpus file sends either message inside a note's own chunk -- but doing
        // nothing is the one answer that is certainly wrong, since it leaves the note sounding
        // after a message whose whole purpose is to stop it.
        for (PendingNote& queued : pending_notes) {
            if (queued.part == channel) {
                queued.released = true;
                queued.choke = controller == 120;
                queued.release_damper = 0;
            }
        }
        part.sustained.clear();
        part.sostenuto_down = false;
        part.sostenuto_captured.clear();
        part.sostenuto_released.clear();
        break;

    case 121:
        if (value != 0) {
            break;
        }
        part.reset_controllers();
        // The damper is up now, so anything it was holding releases -- the engine's `caseD_79`
        // runs `part_sustain_release` for exactly this.
        release_sustained(channel, part);
        break;

    // Mono On accepts a voice count of up to sixteen; Poly On takes only zero. Either mode change
    // flushes what the part is sounding.
    case 126:
        if (value <= 0x10) {
            part.mono = true;
            flush_part_voices(channel);
        }
        break;
    case 127:
        if (value == 0) {
            part.mono = false;
            flush_part_voices(channel);
        }
        break;

    default:
        break;
    }
}

void ToneGenerator::Impl::commit_data_entry_msb(int channel, Part& part, int value)
{
    // Data entry commits whichever of RPN or NRPN was selected last.
    if (part.data_entry_is_nrpn) {
        if (!part.rx.nrpn) {
            return;
        }
        switch (part.nrpn_msb) {
        // 01 xx -- the part modify offsets, the same bytes the sound controllers write.
        case 0x01:
            switch (part.nrpn_lsb) {
            case 0x08:
                part.vibrato_rate = value;
                break;
            case 0x09:
                part.vibrato_depth = value;
                break;
            case 0x0A:
                part.vibrato_delay = value;
                break;
            case 0x20:
                part.tvf_cutoff = value;
                break;
            case 0x21:
                part.tvf_resonance = value;
                break;
            case 0x63:
                part.env_attack = value;
                break;
            case 0x64:
                part.env_decay = value;
                break;
            case 0x66:
                part.env_release = value;
                break;
            default:
                break;
            }
            break;

        // 1x rr -- the drum planes, keyed by the NRPN LSB. The engine applies these only on a
        // rhythm part (`nrpn_apply` tests the part's drum flag before every plane write).
        case 0x18:
            if (is_drum_part(channel)) {
                drum_setup[kit_slot(channel)].set_pitch(part.nrpn_lsb, value);
            }
            break;
        case 0x1A:
            if (is_drum_part(channel)) {
                drum_setup[kit_slot(channel)].set_level(part.nrpn_lsb, value);
            }
            break;
        case 0x1C:
            if (is_drum_part(channel)) {
                drum_setup[kit_slot(channel)].set_pan(part.nrpn_lsb, value);
            }
            break;
        case 0x1D:
            if (is_drum_part(channel)) {
                drum_setup[kit_slot(channel)].set_reverb(part.nrpn_lsb, value);
            }
            break;
        case 0x1E:
            if (is_drum_part(channel)) {
                drum_setup[kit_slot(channel)].set_chorus(part.nrpn_lsb, value);
            }
            break;
        case 0x1F:
            if (is_drum_part(channel)) {
                drum_setup[kit_slot(channel)].set_delay(part.nrpn_lsb, value);
            }
            break;
        default:
            break;
        }
        return;
    }

    if (!part.rx.rpn || part.rpn_is_null() || part.rpn_msb != 0) {
        return;
    }
    switch (part.rpn_lsb) {
    case 0:
        part.bend_range = value;
        break;
    case 1:
        part.fine_tune = (value << 7) | (part.fine_tune & 0x7F);
        break;
    case 2:
        part.coarse_tune = value;
        break;
    default:
        break;
    }
}

void ToneGenerator::Impl::commit_data_entry_lsb(Part& part, int value)
{
    // Only the RPN fine tune keeps its LSB: the bend range ignores cents on this module, and the
    // NRPN targets are all single-byte.
    if (part.data_entry_is_nrpn || !part.rx.rpn || part.rpn_is_null()) {
        return;
    }
    if (part.rpn_msb == 0 && part.rpn_lsb == 1) {
        part.fine_tune = (part.fine_tune & 0x3F80) | (value & 0x7F);
    }
}

ToneGenerator::StateReplay::StateReplay(ToneGenerator& generator) noexcept : generator_(&generator)
{
    generator_->impl_->replaying_state = true;
}

ToneGenerator::StateReplay::~StateReplay()
{
    generator_->impl_->replaying_state = false;
}

} // namespace ts
