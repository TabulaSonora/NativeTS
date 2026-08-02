#include "tabulasonora/tone_generator.hpp"

#include "dsp/simd.hpp"
#include "realtime/partial_voice.hpp"
#include "tabulasonora/send_effects.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>

namespace ts {

struct ToneGenerator::Impl {
    Impl(NoteRenderer& renderer, const ToneGeneratorOptions& opts) : notes(&renderer), options(opts)
    {
        output_gain = opts.output_gain;
        reverb_type = opts.reverb_type;
        chorus_type = opts.chorus_type;
        delay_type = opts.delay_type;

        pool.stealing = [this](int index) { steal(index); };
    }

    NoteRenderer* notes;
    ToneGeneratorOptions options;

    VoicePool pool;
    std::array<Part, ToneGenerator::part_count> parts;

    /// One voice per slot, or empty. Recycled through `spare` rather than reallocated per note.
    std::array<std::unique_ptr<PartialVoice>, VoicePool::max_voices> slots;
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

    // How much of the current block has been handed out. Blocks are always rendered whole, whatever
    // the caller asks for, because a voice's control tick is counted in them.
    int block_offset = block_size;

    std::optional<Reverb> reverb;
    std::optional<Chorus> chorus;
    std::optional<SystemDelay> delay;
    std::optional<int> reverb_type;
    std::optional<int> chorus_type;
    std::optional<int> delay_type;

    // What each effect was last built for. Distinct from the selection above so that reselecting
    // the type a network already has does not throw its tail away.
    std::optional<int> reverb_built_for = -1;
    std::optional<int> chorus_built_for = -1;
    std::optional<int> delay_built_for = -1;

    double output_gain = 1.0;
    std::optional<int> drum_map_row;
    // One per port: each port's channel 10 is its own drum part, with its own kit.
    std::array<int, ToneGenerator::port_count> drum_kit{};
    std::int64_t position = 0;
    int note_count = 0;

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
    [[nodiscard]] static constexpr int part_of(int port, int channel) noexcept
    {
        return ((port & (ToneGenerator::port_count - 1)) * Sequence::channel_count) + channel;
    }

    // Every port has its own drum part, on the same channel within that port.
    [[nodiscard]] bool is_drum_part(int part) const noexcept
    {
        return part % Sequence::channel_count == options.drum_channel;
    }

    void control_change(int channel, Part& part, int controller, int value);
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
    return impl_->parts[static_cast<std::size_t>(index)];
}

const VoicePool& ToneGenerator::voices() const noexcept
{
    return impl_->pool;
}

int ToneGenerator::drum_kit() const noexcept
{
    return impl_->drum_kit[0];
}

int ToneGenerator::drum_kit_for(int port) const noexcept
{
    return impl_->drum_kit[static_cast<std::size_t>(port & (port_count - 1))];
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

void ToneGenerator::reset()
{
    for (int i = 0; i < VoicePool::max_voices; ++i) {
        impl_->recycle(i);
    }

    for (auto& voice : impl_->dying) {
        voice->kill();
        impl_->spare.push_back(std::move(voice));
    }
    impl_->dying.clear();
    impl_->pool.reset();

    for (Part& part : impl_->parts) {
        part.reset();
    }

    if (impl_->reverb) {
        impl_->reverb->reset();
    }
    if (impl_->chorus) {
        impl_->chorus->reset();
    }
    if (impl_->delay) {
        impl_->delay->reset();
    }

    impl_->drum_kit.fill(0);
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
    const int channel = Impl::part_of(port, status & 0x0F);
    Part& part = impl_->parts[static_cast<std::size_t>(channel)];

    switch (status & 0xF0) {
    case 0x90:
        if (data2 > 0) {
            // Re-striking a still-open note takes the old voice first.
            impl_->stop_note(channel, data1);

            // The new strike supersedes a note-off the pedal is still holding for this note.
            std::erase(part.sustained, data1);
            std::erase(part.sostenuto_captured, data1);
            std::erase(part.sostenuto_released, data1);

            impl_->start_note(channel, data1, data2);
            break;
        }
        [[fallthrough]];

    case 0x80:
        if (part.sostenuto_down
            && std::find(part.sostenuto_captured.begin(), part.sostenuto_captured.end(), data1)
                   != part.sostenuto_captured.end()) {
            // A captured note's release is deferred until the sostenuto pedal lifts.
            part.sostenuto_released.push_back(data1);
        } else if (part.damper_down()) {
            part.sustained.push_back(data1);
        } else {
            impl_->stop_note(channel, data1, part.damper);
        }
        break;

    case 0xB0:
        impl_->control_change(channel, part, data1, data2);
        break;

    case 0xC0: {
        part.program = data1;
        if (impl_->is_drum_part(channel)) {
            const std::optional<int> kit =
                impl_->notes->drums().kit_for_program(data1, impl_->effective_drum_map_row());
            if (kit) {
                // An undefined program leaves the current kit in place rather than falling back to
                // Standard.
                impl_->drum_kit[static_cast<std::size_t>(channel / Sequence::channel_count)] = *kit;
            }
        }
        break;
    }

    case 0xE0:
        part.bend = data1 | (data2 << 7);
        break;

    default:
        break;
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

    // Roland GS DT1: F0 41 dev 42 12 <3-byte address> <data> checksum F7.
    if (bytes.size() < 11 || bytes[0] != 0xF0 || bytes[1] != 0x41 || bytes[3] != 0x42
        || bytes[4] != 0x12) {
        return;
    }

    const int a1 = bytes[5];
    const int a2 = bytes[6];
    const int a3 = bytes[7];
    const int value = bytes[8];

    if (a1 == 0x40 && a2 == 0x01) {
        if ((a3 == 0x30 || a3 == 0x31) && value <= 7) {
            impl_->reverb_type =
                impl_->options.reverb_type ? impl_->options.reverb_type : std::optional{value};
        } else if (a3 == 0x38 && value <= 7) {
            impl_->chorus_type =
                impl_->options.chorus_type ? impl_->options.chorus_type : std::optional{value};
        } else if (a3 == 0x50 && value <= 9) {
            impl_->delay_type =
                impl_->options.delay_type ? impl_->options.delay_type : std::optional{value};
        }
    } else if (a1 == 0x40 && (a2 & 0xF0) == 0x10 && a3 == 0x2C) {
        impl_
            ->parts[static_cast<std::size_t>(
                Impl::part_of(port, sequence_builder::channel_from_block(a2 & 0x0F)))]
            .delay_send = value;
    }
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

    for (int i = 0; i < VoicePool::max_voices; ++i) {
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

    mix_effects(left, right);
}

void ToneGenerator::Impl::mix_voice(PartialVoice& voice,
                                    std::span<float> left,
                                    std::span<float> right)
{
    const Part& part = parts[static_cast<std::size_t>(voice.channel())];
    std::span<float> block{scratch};

    voice.render(block, part.bend_milli_semitones(), part.mod_wheel_depth());

    // A silenced channel contributes nothing at all -- not to the dry mix and not to the sends
    // either, so muting a part also removes its tail. It keeps running, so unmuting is instant.
    // The mask is sixteen wide and indexed by channel, so muting a channel silences it on both
    // ports -- one mixer strip per channel rather than per part.
    if (options.channels != nullptr
        && !options.channels->is_audible(voice.channel() % Sequence::channel_count)) {
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
    simd::mix_scaled(block, gain, pan_left, left);
    simd::mix_scaled(block, gain, pan_right, right);

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

    const auto add = [&] {
        simd::add(wet_left, left);
        simd::add(wet_right, right);
    };

    // The same order the offline renderer mixes in: chorus, delay, then reverb.
    if (chorus) {
        chorus->process(chorus_bus, wet_left, wet_right);
        add();
    }
    if (delay) {
        delay->process(delay_bus, wet_left, wet_right);
        add();
    }
    if (reverb) {
        reverb->process(reverb_bus, wet_left, wet_right);
        add();
    }
}

void ToneGenerator::Impl::ensure_effects()
{
    // A type change mid-stream replaces the network, which clears its tail. The offline renderer
    // cannot do this at all -- it picks the last type the file selects and runs the whole song
    // through it -- so a file that switches types part way sounds different here, and closer to the
    // module.
    if (options.reverb && (!reverb || reverb_built_for != reverb_type)) {
        reverb.emplace(Reverb::for_type(reverb_type));
        reverb_built_for = reverb_type;
    }
    if (options.chorus && (!chorus || chorus_built_for != chorus_type)) {
        chorus.emplace(Chorus::for_type(chorus_type));
        chorus_built_for = chorus_type;
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
                                     rate_key);

    TvfChain::Envelope cutoff = notes->tvf().create_envelope(partial, velocity, key, sample_rate);

    return Envelopes{std::move(amplitude), std::move(cutoff.offsets), cutoff.base_cutoff};
}

void ToneGenerator::Impl::start_note(int channel, int note, int velocity)
{
    if (is_drum_part(channel)) {
        start_drum(channel, note, velocity);
        return;
    }

    Part& part = parts[static_cast<std::size_t>(channel)];
    const std::vector<int> tones =
        notes->directory().program_tones(part.program, options.map, part.bank);
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
            Envelopes built = envelopes(tone_number, partial, key, velocity);
            auto [lfo1, lfo2] = notes->lfo().create_runners(tone_number, partial);

            const double base_pitch =
                notes->pitch().base_pitch_milli_semitones(partial, note, partial.key_center());

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
            setup.auto_release_samples = -1;

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

    // The kit's own key is kept alongside the overridden one, because the envelope rate key-follow
    // indexes off the stored plane rather than the plane the override has already doubled the NRPN
    // offset into.
    const DrumKey kit_key = notes->drums().key(
        note, drum_kit[static_cast<std::size_t>(channel / Sequence::channel_count)]);
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
    const auto ring = static_cast<std::int64_t>(options.drum_ring_seconds
                                                * NoteRenderer::drum_ring_scale(key) * sample_rate);
    const int group = pool.begin_note_group();
    bool sounded = false;

    for (const ResolvedPartial& sounding : resolved.partials) {
        const PartialParameters& partial =
            tone->partials()[static_cast<std::size_t>(sounding.partial_index)];
        const DecodedWave* wave = notes->sampler().decode(sounding.descriptor);
        if (wave == nullptr) {
            continue;
        }

        Envelopes built = envelopes(key.tone, partial, 60, velocity, rate_key);
        auto [lfo1, lfo2] = notes->lfo().create_runners(key.tone, partial);

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
        setup.auto_release_samples = ring;

        begin(channel, note, velocity, group, std::move(setup));
        sounded = true;
    }

    if (sounded) {
        ++note_count;
    }
}

void ToneGenerator::Impl::control_change(int channel, Part& part, int controller, int value)
{
    switch (controller) {
    case 0:
        part.bank = value;
        break;
    case 1:
        part.modulation = value;
        break;
    case 7:
        part.set_volume(value);
        break;
    // CC#10 zero is stored as one, so the wheel cannot reach the random position: only the GS
    // SysEx panpot writes a true zero, which is what RND is.
    case 10:
        part.pan = value == 0 ? 1 : value;
        break;
    case 11:
        part.set_expression(value);
        break;
    case 91:
        part.reverb_send = value;
        break;
    case 93:
        part.chorus_send = value;
        break;

    case 64:
        part.damper = value;
        if (value < 0x40) {
            // Oldest first, which is the order the offline renderer closes them in too. The pedal
            // value at the lift reaches the release rate on half-damper tones, so a pedal eased up
            // through 1-0x3f gives those notes a proportionally longer tail.
            const std::vector<int> releasing = part.sustained;
            part.sustained.clear();
            for (int note : releasing) {
                stop_note(channel, note, value);
            }
        }
        break;

    case 5:
        part.portamento_time = value;
        break;
    case 126:
        part.mono = true;
        break;
    case 127:
        part.mono = false;
        break;
    case 65:
        part.portamento_on = value >= 0x40;
        break;
    case 84:
        part.portamento_control_key = value;
        break;

    case 66:
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
        // Data entry commits whichever of RPN or NRPN was selected last.
        if (part.data_entry_is_nrpn) {
            switch (part.nrpn_msb) {
            case 0x18:
                part.drum_keys.set_pitch(part.nrpn_lsb, value);
                break;
            case 0x1C:
                part.drum_keys.set_pan(part.nrpn_lsb, value);
                break;
            default:
                break;
            }
        } else if (part.rpn_msb == 0 && part.rpn_lsb == 0) {
            part.bend_range = value;
        }
        break;

    case 120:
    case 123:
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
        part.reset_controllers();
        break;

    default:
        break;
    }
}

} // namespace ts
