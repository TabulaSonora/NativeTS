#include "tabulasonora/tone_generator.hpp"

#include "dsp/simd.hpp"
#include "realtime/partial_voice.hpp"
#include "tabulasonora/control_decode.hpp"
#include "tabulasonora/effect_programmer.hpp"
#include "tabulasonora/equalizer.hpp"
#include "tabulasonora/send_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
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
        drum_kit.assign(static_cast<std::size_t>(port_count) * 2, 0);
        for (std::size_t i = 0; i < parts.size(); ++i) {
            parts[i].rx_channel = static_cast<int>(i) % Sequence::channel_count;
        }
    }

    NoteRenderer* notes;
    ToneGeneratorOptions options;

    /// Ports this engine runs, always a power of two so `part_of` can mask.
    int port_count = ToneGenerator::port_count;

    VoicePool pool;
    std::vector<Part> parts;

    /// One voice per slot, or empty. Recycled through `spare` rather than reallocated per note.
    /// One voice per slot, sized to the pool. A growing pool resizes this alongside itself.
    std::vector<std::unique_ptr<PartialVoice>> slots;
    std::vector<std::unique_ptr<PartialVoice>> spare;
    /// Voices taken from their slot by stealing, still fading out.
    std::vector<std::unique_ptr<PartialVoice>> dying;

    std::array<float, block_size> scratch{};
    std::array<float, block_size> reverb_bus{};
    std::array<float, block_size> chorus_bus{};
    std::array<float, block_size> delay_bus{};
    std::array<float, block_size> wet_left{};
    std::array<float, block_size> wet_right{};
    std::array<float, block_size> block_left{};
    std::array<float, block_size> block_right{};

    /// The dry path of the parts that switched the EQ on, kept apart until it has been filtered.
    ///
    /// The engine expresses this as a bus number rather than a buffer: a voice's dry destination is
    /// bus `0x33` when its part has the EQ on and `0x3a` when it does not. The sends are untouched
    /// either way -- only the dry path detours.
    std::array<float, block_size> eq_left{};
    std::array<float, block_size> eq_right{};

    // How much of the current block has been handed out. Blocks are always rendered whole, whatever
    // the caller asks for, because a voice's control tick is counted in them.
    int block_offset = block_size;

    std::optional<Reverb> reverb;
    std::optional<Chorus> chorus;
    std::optional<SystemDelay> delay;

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
    bool reverb_row_edited = false;
    bool chorus_row_edited = false;

    /// Wet levels, `40 01 33` and `40 01 3A`, which are not coefficients.
    ///
    /// They scale what leaves each network rather than shaping it, so they are applied at the mix
    /// and not folded into a preset. 0x40 is the power-on value and means unity.
    int reverb_level = 0x40;
    int chorus_level = 0x40;

    [[nodiscard]] static double level_scale(int level) noexcept
    {
        return static_cast<double>(level) / 64.0;
    }

    /// Loads a macro's row, which is what selecting a type does before any edit.
    void load_macro_rows()
    {
        reverb_row = EffectProgrammer::reverb_macro_row(notes->rom(), reverb_type.value_or(4));
        chorus_row = EffectProgrammer::chorus_macro_row(notes->rom(), chorus_type.value_or(2));
        reverb_level = reverb_row[2];
        chorus_level = chorus_row[1];
        reverb_row_edited = false;
        chorus_row_edited = false;
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

    // The (port, map) kit slot a rhythm part reads and its program change writes.
    [[nodiscard]] std::size_t kit_slot(int part) const noexcept
    {
        return static_cast<std::size_t>((part / Sequence::channel_count) * 2
                                        + map_of(parts[static_cast<std::size_t>(part)]));
    }

    // Which vintage's tone map the part resolves against: bank select LSB 1-4 names one, anything
    // else keeps the configured default.
    //
    // XG overrides both. System On puts every part on the XG map, and there is no per-part escape
    // from it while the mode holds -- the bank LSB has become the variation index and no longer
    // names a map at all.
    [[nodiscard]] ToneMap tone_map_for(const Part& part) const noexcept
    {
        if (xg_mode) {
            return ToneMap::xg;
        }
        if (part.bank_lsb >= 1 && part.bank_lsb <= 4) {
            return static_cast<ToneMap>(part.bank_lsb);
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
        if (part.bank_lsb >= 1 && part.bank_lsb <= 4) {
            const std::optional<int> row =
                DrumKitTable::row_for_map(static_cast<ToneMap>(part.bank_lsb));
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
    void xg_effect1(int parameter, std::span<const std::uint8_t> data);
    void leave_xg_mode();
    void gs_drum_setup(int port, int map, int parameter, int key,
                       std::span<const std::uint8_t> data);
    void release_sustained(int channel, Part& part);
    void flush_part_voices(int channel);

    // Returns every part to power-on state without touching the clocks: what a GM System On, GS
    // reset or system-mode-set does mid-stream.
    void stream_reset();
    [[nodiscard]] bool any_voice_on(int channel) const;
    void stop_note(int channel, int note, int damper = 0);
    void start_note(int channel, int note, int velocity);
    void start_drum(int channel, int note, int velocity);
    void begin(int channel, int note, int velocity, int group, VoiceSetup&& setup);

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
    return impl_->drum_kit[0];
}

int ToneGenerator::drum_kit_for(int port) const noexcept
{
    // The MAP1 slot: what the port's default rhythm part carries, which is what a kit display
    // means by "the kit".
    return impl_->drum_kit[static_cast<std::size_t>(port & (impl_->port_count - 1)) * 2];
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
    impl_->master_tune = 0x400;
    impl_->master_key_shift = 0x40;
    impl_->master_pan = 0x40;

    if (impl_->reverb) {
        impl_->reverb->reset();
    }
    if (impl_->chorus) {
        impl_->chorus->reset();
    }
    if (impl_->delay) {
        impl_->delay->reset();
    }

    std::fill(impl_->drum_kit.begin(), impl_->drum_kit.end(), 0);
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
    if (message.kind == MidiEventKind::sysex) {
        if (!message.sysex.empty()) {
            send_sysex(port, message.sysex);
        }
        return;
    }
    send_channel(port, message.status, message.data1, message.data2);
}

void ToneGenerator::send_channel(int status, int data1, int data2)
{
    send_channel(0, status, data1, data2);
}

void ToneGenerator::send_channel(int port, int status, int data1, int data2)
{
    // Parts are matched by their receive channel rather than indexed by it, the way the engine
    // walks a per-channel list of listening parts: SysEx can point several parts at one channel,
    // or detach a part entirely.
    const int incoming = status & 0x0F;
    const int base = (port & (impl_->port_count - 1)) * Sequence::channel_count;
    for (int i = 0; i < Sequence::channel_count; ++i) {
        const int index = base + i;
        Part& part = impl_->parts[static_cast<std::size_t>(index)];
        if (part.rx_channel == incoming) {
            impl_->apply_channel(index, part, status & 0xF0, data1, data2);
        }
    }
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
    if (is_drum_part(part_index)) {
        const std::optional<int> kit = notes->drums().kit_for_program(program, drum_row_for(part));
        if (kit) {
            // An undefined program leaves the current kit in place rather than falling back to
            // Standard.
            drum_kit[kit_slot(part_index)] = *kit;
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
void ToneGenerator::Impl::xg_program_change(int part_index, Part& part, int program)
{
    const int msb = part.xg_bank_msb;
    const bool drum = msb >= 0x7E;

    // The routing decision is the part's, not the channel's, for as long as XG mode holds.
    part.rhythm = drum ? 1 : 0;

    if (!drum) {
        program_change(part_index, part, program);
        return;
    }

    int kit_program = program;
    if (msb == 0x7E) {
        kit_program += 0x78;
    } else if (kit_program > 0x77) {
        kit_program = 0;
    }

    part.program = program;
    const std::optional<int> kit =
        notes->drums().kit_for_program(kit_program, drum_row_for(part));
    if (kit) {
        drum_kit[kit_slot(part_index)] = *kit;
    }
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

    // Part Mode: 0 normal, 1 drum, 3/4/5 the numbered drum setups. Anything drum-shaped routes the
    // part to the drum path and re-resolves its program there.
    case 0x07:
        part.rhythm = value == 0 ? 0 : 1;
        xg_program_change(part_index, part, part.program);
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
    send_sysex(0, bytes);
}

void ToneGenerator::send_sysex(int port, std::span<const std::uint8_t> bytes)
{
    // Universal master volume: F0 7F 7F 04 01 ll mm F7.
    if (bytes.size() >= 8 && bytes[0] == 0xF0 && bytes[1] == 0x7F && bytes[3] == 0x04
        && bytes[4] == 0x01) {
        for (Part& part : impl_->parts) {
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
            if (impl_->xg_mode) {
                impl_->leave_xg_mode();
            } else {
                impl_->stream_reset();
            }
        } else if (bytes[4] == 0x02) {
            impl_->leave_xg_mode();
        }
        return;
    }

    // Yamaha XG: F0 43 1n 4C <3-byte address> <data...> F7.
    if (bytes.size() >= 8 && bytes[0] == 0xF0 && bytes[1] == 0x43) {
        impl_->xg_sysex(bytes);
        return;
    }

    // Roland GS: F0 41 dev 42 <command> <3-byte address> <data...> checksum F7.
    if (bytes.size() < 11 || bytes[0] != 0xF0 || bytes[1] != 0x41 || bytes[3] != 0x42) {
        return;
    }

    // Any Roland message ends XG mode, before it is itself acted on. The module does this without
    // inspecting the message at all, so a file that interleaves the two dialects does not layer
    // them -- it flips the instrument back and forth, resetting every part each time.
    impl_->leave_xg_mode();

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
        impl_->stream_reset();
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
                    impl_->master_tune = std::clamp(raw, 0x18, 0x7E8);
                }
                break;
            case 0x04:
                for (Part& part : impl_->parts) {
                    part.set_master(value);
                }
                break;
            case 0x05:
                // Master key shift, clamped to +-24 semitones (`sysex_master_key_shift`).
                impl_->master_key_shift = std::clamp(value, 0x28, 0x58);
                break;
            case 0x06:
                // Latched but not yet consumed -- the mixer has no master pan stage.
                impl_->master_pan = value;
                break;
            case 0x7F:
                // GS reset (00). The other defined value, 7F, exits GS mode; both return the
                // stream state to power-on.
                impl_->stream_reset();
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
                impl_->reverb_type =
                    impl_->options.reverb_type ? impl_->options.reverb_type : std::optional{value};
                impl_->load_macro_rows();
            } else if (a3 == 0x38 && value <= 7) {
                impl_->chorus_type =
                    impl_->options.chorus_type ? impl_->options.chorus_type : std::optional{value};
                impl_->load_macro_rows();
            } else if (a3 == 0x50 && value <= 9) {
                impl_->delay_type =
                    impl_->options.delay_type ? impl_->options.delay_type : std::optional{value};
            } else if (a3 >= 0x31 && a3 <= 0x37) {
                // Reverb parameters, straight into the row the macro filled in. Level is byte [2]
                // and is kept out of the network -- see `reverb_level`.
                impl_->reverb_row[static_cast<std::size_t>(a3 - 0x31)] =
                    static_cast<std::uint8_t>(value);
                if (a3 == 0x33) {
                    impl_->reverb_level = value;
                } else {
                    impl_->reverb_row_edited = true;
                }
            } else if (a3 >= 0x39 && a3 <= 0x40) {
                impl_->chorus_row[static_cast<std::size_t>(a3 - 0x39)] =
                    static_cast<std::uint8_t>(value);
                if (a3 == 0x3A) {
                    impl_->chorus_level = value;
                } else {
                    impl_->chorus_row_edited = true;
                }
            }
            // Recognised and left alone: the patch name (00-0F) and voice reserve (10-1F) have no
            // audible counterpart here, and the individual reverb (32-37), chorus (39-40) and
            // delay (51-5A) parameters are carried by the preset tables the macros select --
            // applying single-parameter edits on top is follow-up DSP work
            // (`sysex_reverb_params` / `sysex_chorus_params` / `sysex_delay_params`).
            return;
        }

        if (a2 == 0x02) {
            // The four-band EQ block (`sysex_eq_params`): low frequency and gain, then high. Each
            // setter applies the engine's own range test and ignores anything outside it.
            switch (a3) {
            case 0x00:
                impl_->equalizer.set_low_frequency(value);
                break;
            case 0x01:
                impl_->equalizer.set_low_gain(value);
                break;
            case 0x02:
                impl_->equalizer.set_high_frequency(value);
                break;
            case 0x03:
                impl_->equalizer.set_high_gain(value);
                break;
            default:
                break;
            }
            return;
        }

        if (a2 == 0x03) {
            // Insertion EFX (`sysex_insertion_fx_type` / `sysex_insertion_fx_params`): 00-01 pick
            // one of the 67 algorithms, 03 onward are its parameters, and the tail of the block
            // carries the EFX send and control assignments. Placeholder until the insertion chain
            // exists; the spec's own scope note keeps EFX out of this codebase for now.
            return;
        }

        if ((a2 & 0xF0) == 0x10) {
            // Part parameters, port-relative block addressing.
            const int index =
                impl_->part_of(block_port, sequence_builder::channel_from_block(a2 & 0x0F));
            impl_->gs_part_parameter(
                index, impl_->parts[static_cast<std::size_t>(index)], a3, data);
            return;
        }

        if ((a2 & 0xF0) == 0x40) {
            // The extended part block. `20` switches this part through the EQ; `22` is the
            // insertion-EFX assignment, which is recognised and stored against the chain that will
            // read it. `00`/`01` are the SysEx form of the tone map number, which this engine takes
            // from its options and CC#32 instead.
            const int index =
                impl_->part_of(block_port, sequence_builder::channel_from_block(a2 & 0x0F));
            Part& part = impl_->parts[static_cast<std::size_t>(index)];
            if (a3 == 0x20) {
                part.eq_enabled = value != 0;
            }
            return;
        }

        if ((a2 & 0xF0) == 0x20) {
            // The controller assignment matrix (`sysex_part_control_matrix`). The address splits
            // into a source in the high nibble and a destination in the low one.
            const int index =
                impl_->part_of(block_port, sequence_builder::channel_from_block(a2 & 0x0F));
            Part& part = impl_->parts[static_cast<std::size_t>(index)];

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

    if (a1 == 0x41 || a1 == 0x51) {
        // Drum setup: a2 is (map << 4) | parameter, a3 the first key. The map nibble picks which
        // of the two per-map setup buffers the module writes -- bit 12 of the address index, so
        // only its low bit counts -- and parts read the buffer of the map they are assigned to.
        impl_->gs_drum_setup(block_port, (a2 >> 4) & 1, a2 & 0x0F, a3, data);
        return;
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

    case 0x2C:
        part.delay_send = value;
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

        for (std::size_t offset = 0; offset < data.size(); ++offset) {
            const int note = key + static_cast<int>(offset);
            const int value = data[offset];
            switch (parameter) {
            case 0x00:
                // Drum set name -- nothing to display it on.
                break;
            case 0x01:
                // Play key number: absolute, unlike the relative pitch NRPN.
                part.drum_keys.set_play_key(note, value);
                break;
            case 0x02:
                part.drum_keys.set_level(note, value);
                break;
            case 0x03:
                // Assign group, clamped to the engine's 1-4 (`drum_setup_assign_group`).
                part.drum_keys.set_group(note, std::clamp(value, 1, 4));
                break;
            case 0x04:
                part.drum_keys.set_pan(note, value);
                break;
            case 0x05:
                part.drum_keys.set_reverb(note, value);
                break;
            case 0x06:
                part.drum_keys.set_chorus(note, value);
                break;
            case 0x07:
                // Rx note-off per key: bit 0 of the module's per-key flag byte, seeded by the
                // kit record and rewritten here (`drum_setup_rx_noteoff`). Nonzero engages, as
                // the module's own write does.
                part.drum_keys.set_rx_note_off(note, value != 0);
                break;
            case 0x08:
                // Rx note-on per key: bit 4 of the same byte (`drum_setup_rx_noteon`). A key
                // switched off here does not sound at all -- the module's note-on dispatch
                // refuses it before velocity or mute groups are even considered.
                part.drum_keys.set_rx_note_on(note, value != 0);
                break;
            case 0x09:
                part.drum_keys.set_delay(note, value);
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
    }

    reverb_type = options.reverb_type;
    chorus_type = options.chorus_type;
    delay_type = options.delay_type;

    master_tune = 0x400;
    master_key_shift = 0x40;
    master_pan = 0x40;
    std::fill(drum_kit.begin(), drum_kit.end(), 0);

    // The EQ returns to flat and drops its filter memory with it. Keeping the memory would only
    // matter if a later message switched the EQ back on, and what it would then contribute is one
    // block of a signal from before the reset -- so dropping it is both simpler and righter.
    equalizer.reset();
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
    std::span<float> left{block_left};
    std::span<float> right{block_right};

    std::fill(left.begin(), left.end(), 0.0F);
    std::fill(right.begin(), right.end(), 0.0F);
    reverb_bus.fill(0.0F);
    chorus_bus.fill(0.0F);
    delay_bus.fill(0.0F);
    eq_left.fill(0.0F);
    eq_right.fill(0.0F);

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
    // than having written straight through costs nothing when no part has the EQ on: the buffers
    // are cleared to +0 and adding +0 to a float leaves it exactly as it was, so a stream that
    // never touches `40 4x 20` renders identically to one compiled without any of this.
    equalizer.process(eq_left, eq_right);
    for (int n = 0; n < block_size; ++n) {
        left[static_cast<std::size_t>(n)] += eq_left[static_cast<std::size_t>(n)];
        right[static_cast<std::size_t>(n)] += eq_right[static_cast<std::size_t>(n)];
    }

    mix_effects(left, right);
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
    // already sounding.
    voice.set_cutoff_offset(part.modifiers().cutoff_offset());
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

    const auto [pan_left, pan_right] = voice.pan_gains(part.pan);
    const double gain = part.volume_scale() * voice.level_gain();

    // Sends are fed from the pre-pan mono and are post-fader, so the wet scales with volume and
    // expression but not with pan -- matching the engine's mono send bus.
    const double to_reverb =
        reverb && part.reverb_send > 0 ? Reverb::send_gain(part.reverb_send) : 0.0;
    const double to_chorus =
        chorus && part.chorus_send > 0 ? Chorus::send_gain(part.chorus_send) : 0.0;
    const double to_delay =
        delay && part.delay_send > 0 ? SystemDelay::send_gain(part.delay_send) : 0.0;

    // The voice gain stays a separate multiply from the pan and send levels: collapsing them would
    // be the cheaper kernel and a silently different render, since float multiplication is no more
    // associative than addition is.
    //
    // A part with the EQ on lands in the EQ buffers instead of the output, and is filtered and
    // summed in later. Only the dry path moves; the sends below are fed the same either way.
    std::span<float> dry_left = part.eq_enabled ? std::span<float>{eq_left} : left;
    std::span<float> dry_right = part.eq_enabled ? std::span<float>{eq_right} : right;
    simd::mix_scaled(block, gain, pan_left, dry_left);
    simd::mix_scaled(block, gain, pan_right, dry_right);

    // A send at zero is skipped rather than multiplied out, which drops three of the five passes
    // over the block on a part with no sends. That is exact rather than merely harmless: the buses
    // are cleared to +0 at the top of every block, and a sum that starts at +0 can never reach -0,
    // so adding the +-0 the multiply would have produced leaves every element as it stands.
    if (to_reverb != 0.0) {
        simd::mix_scaled(block, gain, to_reverb, reverb_bus);
    }
    if (to_chorus != 0.0) {
        simd::mix_scaled(block, gain, to_chorus, chorus_bus);
    }
    if (to_delay != 0.0) {
        simd::mix_scaled(block, gain, to_delay, delay_bus);
    }
}

void ToneGenerator::Impl::mix_effects(std::span<float> left, std::span<float> right)
{
    ensure_effects();

    // `scale` is the network's wet level, which is not part of its coefficients: it scales what
    // leaves the network rather than shaping it. Unity is skipped rather than multiplied out, so a
    // stream that never edits a level renders exactly as it did before levels existed.
    const auto add = [&](double scale) {
        if (scale == 1.0) {
            simd::add(wet_left, left);
            simd::add(wet_right, right);
            return;
        }
        for (int n = 0; n < block_size; ++n) {
            left[static_cast<std::size_t>(n)] +=
                static_cast<float>(wet_left[static_cast<std::size_t>(n)] * scale);
            right[static_cast<std::size_t>(n)] +=
                static_cast<float>(wet_right[static_cast<std::size_t>(n)] * scale);
        }
    };

    // The same order the module mixes in: chorus, delay, then reverb.
    if (chorus) {
        chorus->process(chorus_bus, wet_left, wet_right);
        add(level_scale(chorus_level));
    }
    if (delay) {
        delay->process(delay_bus, wet_left, wet_right);
        add(1.0);
    }
    if (reverb) {
        reverb->process(reverb_bus, wet_left, wet_right);
        add(level_scale(reverb_level));
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
        chorus.emplace(chorus_row_edited
                           ? Chorus{EffectProgrammer::chorus_from_row(notes->rom(), chorus_row)}
                           : Chorus::for_type(chorus_type));
        chorus_built_for = chorus_type;
        chorus_row_edited = false;
    }
    if (options.delay && (!delay || delay_built_for != delay_type)) {
        delay.emplace(SystemDelay::for_type(delay_type.value_or(0)));
        delay_built_for = delay_type;
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

void ToneGenerator::Impl::begin(int channel, int note, int velocity, int group, VoiceSetup&& setup)
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

    std::unique_ptr<PartialVoice> voice;
    if (!spare.empty()) {
        voice = std::move(spare.back());
        spare.pop_back();
    } else {
        voice = std::make_unique<PartialVoice>(notes->interpolator(), notes->tvf(), notes->pan());
    }

    voice->start(std::move(setup));
    slots[static_cast<std::size_t>(slot.index)] = std::move(voice);
}

bool ToneGenerator::Impl::any_voice_on(int channel) const
{
    for (const auto& slot : slots) {
        if (slot && !slot->finished() && slot->channel() == channel) {
            return true;
        }
    }
    return false;
}

void ToneGenerator::Impl::stop_note(int channel, int note, int damper)
{
    for (auto& slot : slots) {
        if (slot && slot->channel() == channel && slot->note() == note && !slot->released()) {
            slot->note_off(damper);
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
    if (note < part.key_low || note > part.key_high) {
        return;
    }

    // Velocity sense, applied here rather than deeper because the engine applies it once at
    // note-on and everything downstream reads the result -- the level chain, both envelope
    // velocity scales and the filter's own velocity term all take the sensed value, not the
    // value that arrived on the wire.
    velocity = part.effective_velocity(velocity);

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

    // A part panpot of zero is GS RND: the engine repositions the note outright rather than
    // offsetting the partial's own pan, and redraws for every note. Only the SysEx panpot can set
    // it -- CC#10 clamps zero to one, so the wheel cannot reach this.
    const std::optional<int> random_pan =
        part.pan == 0 ? std::optional{notes->noise().next_pan()} : std::nullopt;

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

    for (int tone_number : tones) {
        const ResolvedTone resolved = notes->directory().resolve(tone_number, note, velocity);
        const std::optional<Tone> tone = notes->directory().tone(tone_number);
        if (!tone) {
            continue;
        }

        for (const ResolvedPartial& sounding : resolved.partials) {
            const PartialParameters& partial =
                tone->partials()[static_cast<std::size_t>(sounding.partial_index)];
            const DecodedWave* wave = notes->sampler().decode(sounding.descriptor);
            if (wave == nullptr) {
                continue;
            }

            const int key = std::clamp(note, 0, 0x7F);
            Envelopes built = envelopes(tone_number, partial, key, velocity, part.modifiers());
            auto [lfo1, lfo2] = notes->lfo().create_runners(tone_number, partial, part.modifiers());

            // Scale tuning folds in here rather than riding with the bend: it is per-key, so it
            // is latched at note-on like the rest of the note's pitch.
            const double base_pitch =
                notes->pitch().base_pitch_milli_semitones(partial, note, partial.key_center())
                + part.scale_offset_milli_semitones(key);

            VoiceSetup setup;
            setup.channel = channel;
            setup.note = note;
            setup.wave = wave;
            setup.partial = partial;
            setup.descriptor = sounding.descriptor;
            setup.amplitude = std::move(built.amplitude);
            setup.cutoff = std::move(built.cutoff);
            setup.cutoff_base = built.cutoff_base;
            setup.pitch_envelope = notes->pitch().create_envelope_runner(partial, key, velocity);
            setup.lfo1 = std::move(lfo1);
            setup.lfo2 = std::move(lfo2);
            setup.envelope_hold_samples = notes->envelopes().hold_samples(partial, velocity);
            setup.half_damper = notes->directory().half_damper(tone_number);
            setup.glide_milli_semitones =
                glide_from < 0
                    ? 0.0
                    : PitchChain::portamento_offset(glide_from, static_cast<int>(base_pitch));
            setup.glide_step = glide_step;
            setup.base_pitch = base_pitch;
            setup.pan = partial.pan();
            setup.pan_follows_part = true;
            setup.random_pan = random_pan;
            setup.level_gain = 1.0;

            begin(channel, note, velocity, group, std::move(setup));
            sounded = true;
        }
    }

    if (sounded) {
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

    // The drum-setup SysEx planes replace their kit entries outright, before the relative NRPN
    // overrides go on top.
    if (const std::optional<int> play_key = part.drum_keys.play_key(note)) {
        kit_key.pitch = *play_key;
    }
    if (const std::optional<int> level = part.drum_keys.level(note)) {
        kit_key.level = *level;
    }
    if (const std::optional<int> group = part.drum_keys.group(note)) {
        kit_key.group = *group;
    }
    if (const std::optional<bool> rx_off = part.drum_keys.rx_note_off(note)) {
        kit_key.receives_note_off = *rx_off;
    }
    if (const std::optional<bool> rx_on = part.drum_keys.rx_note_on(note)) {
        kit_key.receives_note_on = *rx_on;
    }

    // A key switched off simply does not sound. The module refuses it at the top of its note-on
    // dispatch (`0x480[key] & 0x10`), before velocity, mute groups or anything else runs, so
    // nothing below this line -- not even the choke -- happens for it. Every kit record ships
    // with every sounding key receiving, so only an explicit drum-setup write can get here.
    if (!kit_key.receives_note_on) {
        return;
    }

    const DrumKey key = DrumKeyOverrides::apply(kit_key,
                                                part.drum_keys.pitch_offset(note),
                                                part.drum_keys.pan_for_hit(note, notes->noise()));
    const int rate_key =
        NoteRenderer::envelope_rate_key(kit_key, part.drum_keys.pitch_offset(note));

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
    const int group = pool.begin_note_group();
    bool sounded = false;

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
        auto [lfo1, lfo2] = notes->lfo().create_runners(key.tone, partial, part.modifiers());

        // The note does not transpose the sample: the kit's coarse-pitch plane supplies the key,
        // and the tone's own key-follow decides what a step of it is worth.
        const double native =
            (sounding.descriptor.root_key * 1000.0) + 1024.0 - sounding.descriptor.fine_tune;

        VoiceSetup setup;
        setup.channel = channel;
        setup.note = note;
        setup.wave = wave;
        setup.partial = partial;
        setup.descriptor = sounding.descriptor;
        setup.amplitude = std::move(built.amplitude);
        setup.cutoff = std::move(built.cutoff);
        setup.cutoff_base = built.cutoff_base;
        setup.pitch_envelope = notes->pitch().create_envelope_runner(partial, 60, velocity);
        setup.lfo1 = std::move(lfo1);
        setup.lfo2 = std::move(lfo2);
        setup.envelope_hold_samples = notes->envelopes().hold_samples(partial, velocity);
        setup.is_drum = true;
        setup.drum_base_ratio = std::pow(
            2.0, (PitchChain::drum_pitch_milli_semitones(partial, key.pitch) - native) / 12000.0);
        setup.pan = key.pan;
        setup.pan_follows_part = false;
        setup.level_gain = level_gain;
        setup.mute_group = key.group;
        setup.drum_receives_note_off = key.receives_note_off;

        begin(channel, note, velocity, group, std::move(setup));
        sounded = true;
    }

    if (sounded) {
        ++note_count;
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

    // The sound controllers land on the same per-part modify bytes as the NRPNs and the part
    // SysEx: CC#71 is `part+0x3e7` (resonance), CC#72 `0x3ea` (release), and so on through the
    // recovered `caseD_47`-`caseD_4e` handlers.
    case 71:
        part.tvf_resonance = value;
        break;
    case 72:
        part.env_release = value;
        break;
    case 73:
        part.env_attack = value;
        break;
    case 74:
        part.tvf_cutoff = value;
        break;
    case 75:
        part.env_decay = value;
        break;
    case 76:
        part.vibrato_rate = value;
        break;
    case 77:
        part.vibrato_depth = value;
        break;
    case 78:
        part.vibrato_delay = value;
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
                part.drum_keys.set_pitch(part.nrpn_lsb, value);
            }
            break;
        case 0x1A:
            if (is_drum_part(channel)) {
                part.drum_keys.set_level(part.nrpn_lsb, value);
            }
            break;
        case 0x1C:
            if (is_drum_part(channel)) {
                part.drum_keys.set_pan(part.nrpn_lsb, value);
            }
            break;
        case 0x1D:
            if (is_drum_part(channel)) {
                part.drum_keys.set_reverb(part.nrpn_lsb, value);
            }
            break;
        case 0x1E:
            if (is_drum_part(channel)) {
                part.drum_keys.set_chorus(part.nrpn_lsb, value);
            }
            break;
        case 0x1F:
            if (is_drum_part(channel)) {
                part.drum_keys.set_delay(part.nrpn_lsb, value);
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

} // namespace ts
