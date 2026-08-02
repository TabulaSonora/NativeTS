#include "tabulasonora/sequence_renderer.hpp"

#include "dsp/simd.hpp"
#include "tabulasonora/send_effects.hpp"
#include "tabulasonora/smf_reader.hpp"
#include "tabulasonora/tva_chain.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace ts {

// ---------------------------------------------------------------------------------------------
// ChannelMask
// ---------------------------------------------------------------------------------------------

bool ChannelMask::any_soloed() const noexcept
{
    return std::any_of(soloed_.begin(), soloed_.end(), [](bool s) { return s; });
}

bool ChannelMask::is_audible(int channel) const noexcept
{
    if (channel < 0 || channel >= channel_count) {
        return false;
    }
    // Solo wins outright: once anything is soloed, a mute on a soloed channel does not silence it.
    if (any_soloed()) {
        return soloed_[static_cast<std::size_t>(channel)];
    }
    return !muted_[static_cast<std::size_t>(channel)];
}

bool ChannelMask::is_muted(int channel) const noexcept
{
    return channel >= 0 && channel < channel_count && muted_[static_cast<std::size_t>(channel)];
}

bool ChannelMask::is_soloed(int channel) const noexcept
{
    return channel >= 0 && channel < channel_count && soloed_[static_cast<std::size_t>(channel)];
}

void ChannelMask::set_muted(int channel, bool muted) noexcept
{
    if (channel >= 0 && channel < channel_count) {
        muted_[static_cast<std::size_t>(channel)] = muted;
    }
}

void ChannelMask::set_soloed(int channel, bool soloed) noexcept
{
    if (channel >= 0 && channel < channel_count) {
        soloed_[static_cast<std::size_t>(channel)] = soloed;
    }
}

void ChannelMask::reset() noexcept
{
    muted_.fill(false);
    soloed_.fill(false);
}

bool ChannelMask::is_default() const noexcept
{
    return std::none_of(muted_.begin(), muted_.end(), [](bool m) { return m; }) && !any_soloed();
}

// ---------------------------------------------------------------------------------------------
// SequenceRenderer
// ---------------------------------------------------------------------------------------------

namespace {

/// Indices of the drum notes, ordered by when they strike.
///
/// A *stable* sort by position: two hits landing on the same sample must keep the order the note
/// list reports them in, or the kit carried forward past a program change depends on the sort.
[[nodiscard]] std::vector<std::size_t>
drum_notes_in_time_order(const Sequence& sequence, int drum_channel, std::int64_t before)
{
    std::vector<std::size_t> indices;
    for (std::size_t i = 0; i < sequence.notes.size(); ++i) {
        if (sequence.notes[i].channel == drum_channel && sequence.notes[i].on < before) {
            indices.push_back(i);
        }
    }
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        return sequence.notes[a].on < sequence.notes[b].on;
    });
    return indices;
}

/// Resolves each drum note's kit, carrying the selection forward.
///
/// A program change on the drum part selects a kit through its own lookup. An *undefined* program
/// leaves the current kit in place rather than falling back to Standard, so the choice has to be
/// tracked in note order rather than resolved per note.
[[nodiscard]] std::map<std::size_t, int>
resolve_drum_kits(const NoteRenderer& notes, const Sequence& sequence, const RenderOptions& options)
{
    std::map<std::size_t, int> kits;
    int current = 0;

    for (std::size_t index : drum_notes_in_time_order(
             sequence, options.drum_channel, std::numeric_limits<std::int64_t>::max())) {
        const std::optional<int> resolved = notes.drums().kit_for_program(
            sequence.notes[index].program, options.effective_drum_map_row());
        if (resolved) {
            current = *resolved;
        }
        kits[index] = current;
    }

    return kits;
}

/// When each drum note is choked by a later hit in the same assign group.
///
/// This is what silences an open hi-hat when the closed one is played.
[[nodiscard]] std::map<std::size_t, int> compute_drum_chokes(const NoteRenderer& notes,
                                                             const Sequence& sequence,
                                                             const RenderOptions& options,
                                                             std::int64_t total,
                                                             const std::map<std::size_t, int>& kits)
{
    std::map<std::size_t, int> choke;
    std::map<int, std::pair<std::size_t, std::int64_t>> last_in_group;

    for (std::size_t index : drum_notes_in_time_order(sequence, options.drum_channel, total)) {
        const NoteRecord& note = sequence.notes[index];
        const auto kit = kits.count(index) != 0 ? kits.at(index) : 0;
        const int group = notes.drums().key(note.note, kit).group;
        if (group == 0) {
            continue;
        }

        const auto previous = last_in_group.find(group);
        if (previous != last_in_group.end() && note.on > previous->second.second) {
            choke[previous->second.first] = static_cast<int>(note.on - previous->second.second);
        }

        last_in_group[group] = {index, note.on};
    }

    return choke;
}

/// Applies a choke to a rendered hit.
///
/// The engine's cut is near-instant: full at the strike, most of the way down by 5 ms and silent by
/// 10 ms. Modelled as a short hold followed by a linear fade -- a hard stop instead is an audible
/// click.
void apply_choke(RenderedNote& voice, int cut_at)
{
    constexpr int hold = 128;
    constexpr int fade = 192;

    const auto start = static_cast<std::size_t>(cut_at + hold);
    if (start >= voice.left.size()) {
        return;
    }

    const std::size_t length = std::min<std::size_t>(fade, voice.left.size() - start);
    for (std::size_t i = 0; i < length; ++i) {
        const auto gain = 1.0F - (static_cast<float>(i) / static_cast<float>(length));
        voice.left[start + i] *= gain;
        voice.right[start + i] *= gain;
        voice.mono[start + i] *= gain;
    }

    const auto silent_from = static_cast<std::ptrdiff_t>(start + length);
    std::fill(voice.left.begin() + silent_from, voice.left.end(), 0.0F);
    std::fill(voice.right.begin() + silent_from, voice.right.end(), 0.0F);
    std::fill(voice.mono.begin() + silent_from, voice.mono.end(), 0.0F);
}

/// The part volume curve for one note.
///
/// Volume, expression and master are re-read as the note plays, at the render-block grid the events
/// land on -- not at the 100 Hz control tick, which is ten times too coarse.
[[nodiscard]] std::vector<double>
build_volume_curve(const Sequence& sequence, const NoteRecord& note, std::size_t span)
{
    const PartTimelines& part = sequence.parts[static_cast<std::size_t>(note.channel)];

    std::vector<int> volume(span);
    std::vector<int> expression(span);
    std::vector<int> master(span);

    part.volume.fill(note.on, volume, sequence_builder::default_volume);
    part.expression.fill(note.on, expression, sequence_builder::default_expression);
    sequence.master_volume.fill(note.on, master, sequence_builder::default_master);

    std::vector<double> curve(span);
    std::tuple<int, int, int> last_key{-1, -1, -1};
    double last_value = 0.0;

    for (std::size_t i = 0; i < span; ++i) {
        const std::tuple<int, int, int> key{volume[i], expression[i], master[i]};
        if (key != last_key) {
            last_value =
                TvaChain::part_volume_scale(std::get<0>(key), std::get<1>(key), std::get<2>(key));
            last_key = key;
        }
        curve[i] = last_value;
    }

    return curve;
}

/// The bend curve for one note, or empty when it never bends.
///
/// Bend moves on the render-block grid too. A song that bends continuously carries thousands of
/// changes, so reading it per control tick smears every slide.
[[nodiscard]] std::vector<double>
build_pitch_curve(const Sequence& sequence, const NoteRecord& note, std::size_t span)
{
    const PartTimelines& part = sequence.parts[static_cast<std::size_t>(note.channel)];

    std::vector<int> bend(span);
    std::vector<int> range(span);
    part.bend.fill(note.on, bend, 8192);
    part.bend_range.fill(note.on, range, 2);

    std::vector<double> curve(span);
    bool moved = false;
    for (std::size_t i = 0; i < span; ++i) {
        curve[i] = PitchChain::bend_offset_milli_semitones(bend[i], range[i]);
        moved = moved || curve[i] != 0.0;
    }

    return moved ? curve : std::vector<double>{};
}

/// The mod wheel's vibrato depth per control tick, or empty when the wheel is never touched.
///
/// Per tick rather than per sample: it feeds LFO1's depth, which the engine only re-evaluates on
/// the control tick.
[[nodiscard]] std::vector<double>
build_mod_wheel_curve(const Sequence& sequence, const NoteRecord& note, std::size_t span)
{
    const PartTimelines& part = sequence.parts[static_cast<std::size_t>(note.channel)];
    if (part.modulation.size() == 0) {
        return {};
    }

    const std::size_t ticks = (span / NoteRenderer::control_block) + 1;
    std::vector<double> per_tick(ticks);
    bool touched = false;

    for (std::size_t t = 0; t < ticks; ++t) {
        const std::int64_t at =
            note.on + (static_cast<std::int64_t>(t) * NoteRenderer::control_block);
        const int controller = part.modulation.value_at(at, 0);
        per_tick[t] = LfoEngine::mod_wheel_depth(controller);
        touched = touched || controller > 0;
    }

    return touched ? per_tick : std::vector<double>{};
}

/// The effect type a sequence selected, or nothing when it never did.
[[nodiscard]] std::optional<int> selected(const ControllerTimeline& timeline)
{
    const int value = timeline.value_at(std::numeric_limits<std::int64_t>::max(), -1);
    return value < 0 ? std::nullopt : std::optional{value};
}

/// Runs one effect over its send bus and adds the wet result to the mix.
void mix_wet(Effect& effect,
             std::span<const float> send,
             std::span<float> left,
             std::span<float> right,
             std::vector<float>& wet_left,
             std::vector<float>& wet_right)
{
    // One scratch pair for all three effects. Each process() writes every element it is given, so
    // there is nothing to clear between them, and at a render's full length these are the two
    // largest buffers here -- a fresh pair per effect was ~230 MB on a five-minute song.
    if (wet_left.size() != send.size()) {
        wet_left.assign(send.size(), 0.0F);
        wet_right.assign(send.size(), 0.0F);
    }

    effect.process(send, wet_left, wet_right);
    simd::add(wet_left, left);
    simd::add(wet_right, right);
}

/// Accumulates one note's mono into a send bus at its level.
void accumulate_send(std::vector<float>& bus,
                     std::span<const float> mono,
                     std::int64_t position,
                     std::size_t count,
                     int level,
                     double (*gain_of)(int))
{
    if (bus.empty() || level <= 0) {
        return;
    }
    simd::mix_scaled(mono.first(count),
                     gain_of(level),
                     std::span{bus}.subspan(static_cast<std::size_t>(position), count));
}

} // namespace

RenderResult SequenceRenderer::render_file(const std::filesystem::path& path,
                                           const RenderOptions& options)
{
    return render(sequence_builder::build(smf::read(path, NoteRenderer::sample_rate)), options);
}

RenderResult SequenceRenderer::render(const Sequence& sequence, const RenderOptions& options)
{
    constexpr int rate = NoteRenderer::sample_rate;

    // The render runs to the last event of any kind, not the last note: files routinely close with
    // controller traffic after the music stops, and truncating there clips the tail.
    std::int64_t end = sequence.last_event_position;
    if (options.end_seconds) {
        end = std::min(end, static_cast<std::int64_t>(*options.end_seconds * rate));
    }

    const auto total =
        static_cast<std::int64_t>(static_cast<double>(end) + (options.tail_seconds * rate));
    if (total <= 0) {
        return RenderResult{};
    }

    const auto length = static_cast<std::size_t>(total);
    RenderResult result;
    result.left.assign(length, 0.0F);
    result.right.assign(length, 0.0F);

    std::vector<float> reverb_send =
        options.reverb ? std::vector<float>(length, 0.0F) : std::vector<float>{};
    std::vector<float> chorus_send =
        options.chorus ? std::vector<float>(length, 0.0F) : std::vector<float>{};
    std::vector<float> delay_send =
        options.delay ? std::vector<float>(length, 0.0F) : std::vector<float>{};

    const std::map<std::size_t, int> kits = resolve_drum_kits(*notes_, sequence, options);
    const std::map<std::size_t, int> choke_at =
        compute_drum_chokes(*notes_, sequence, options, total, kits);

    for (std::size_t index = 0; index < sequence.notes.size(); ++index) {
        const NoteRecord& note = sequence.notes[index];
        if (note.on >= total) {
            continue;
        }
        if (options.channels != nullptr && !options.channels->is_audible(note.channel)) {
            continue;
        }

        const bool is_drum = note.channel == options.drum_channel;
        const double hold = std::max(0.0, static_cast<double>(note.off - note.on) / rate);

        // A pitched-down key rings proportionally longer, so the window has to follow it or the hit
        // is cut off mid-decay.
        const int kit = kits.count(index) != 0 ? kits.at(index) : 0;
        const double drum_ring =
            is_drum ? options.drum_ring_seconds
                          * notes_->drum_ring_scale(note.note, kit, note.drum_pitch)
                    : 0.0;

        const auto span = static_cast<std::int64_t>(std::min<double>(
            static_cast<double>(total - note.on),
            (is_drum ? drum_ring + options.drum_tail_seconds : hold + options.tail_seconds)
                * rate));

        if (span <= 0) {
            continue;
        }

        const std::vector<double> volume =
            build_volume_curve(sequence, note, static_cast<std::size_t>(span));

        RenderedNote voice;
        if (is_drum) {
            // A drum rings out: note-off is ignored, so the ring is a fixed length rather than the
            // note's own duration, and the tone's envelope does the decay.
            voice = notes_->render_drum_note(note.note,
                                             note.velocity,
                                             options.drum_ring_seconds,
                                             options.drum_tail_seconds,
                                             kit,
                                             volume,
                                             note.drum_pitch,
                                             note.drum_pan);

            if (const auto cut = choke_at.find(index); cut != choke_at.end()) {
                apply_choke(voice, cut->second);
            }
        } else {
            const std::vector<double> pitch =
                build_pitch_curve(sequence, note, static_cast<std::size_t>(span));
            const std::vector<double> wheel =
                build_mod_wheel_curve(sequence, note, static_cast<std::size_t>(span));

            voice = notes_->render_note(note.program,
                                        note.note,
                                        note.velocity,
                                        hold,
                                        options.tail_seconds,
                                        options.map,
                                        note.bank,
                                        note.pan,
                                        NoteRenderer::Controllers{volume, pitch, wheel});
        }

        ++result.note_count;

        const auto count =
            std::min<std::size_t>(voice.left.size(), static_cast<std::size_t>(total - note.on));
        const auto at = static_cast<std::size_t>(note.on);

        simd::add(std::span{voice.left}.first(count), std::span{result.left}.subspan(at, count));
        simd::add(std::span{voice.right}.first(count), std::span{result.right}.subspan(at, count));

        accumulate_send(
            reverb_send, voice.mono, note.on, count, note.reverb_send, &Reverb::send_gain);
        accumulate_send(
            chorus_send, voice.mono, note.on, count, note.chorus_send, &Chorus::send_gain);
        accumulate_send(
            delay_send, voice.mono, note.on, count, note.delay_send, &SystemDelay::send_gain);
    }

    // Effects last, in the order the engine runs them: chorus, delay, reverb.
    std::vector<float> wet_left;
    std::vector<float> wet_right;

    if (!chorus_send.empty() && simd::any_non_zero(chorus_send)) {
        Chorus effect = Chorus::for_type(options.chorus_type ? options.chorus_type
                                                             : selected(sequence.chorus_type));
        mix_wet(effect, chorus_send, result.left, result.right, wet_left, wet_right);
    }

    if (!delay_send.empty() && simd::any_non_zero(delay_send)) {
        const std::optional<int> type =
            options.delay_type ? options.delay_type : selected(sequence.delay_type);
        SystemDelay effect = SystemDelay::for_type(type.value_or(0));
        mix_wet(effect, delay_send, result.left, result.right, wet_left, wet_right);
    }

    if (!reverb_send.empty() && simd::any_non_zero(reverb_send)) {
        Reverb effect = Reverb::for_type(options.reverb_type ? options.reverb_type
                                                             : selected(sequence.reverb_type));
        mix_wet(effect, reverb_send, result.left, result.right, wet_left, wet_right);
    }

    // The trim goes on before the peak is measured, so the number a caller reports is the peak of
    // what it will actually write rather than of an intermediate it never sees.
    if (options.output_gain != 1.0) {
        simd::scale(result.left, options.output_gain);
        simd::scale(result.right, options.output_gain);
    }

    // One peak across both channels: maximum is exact under any association, so folding the right
    // channel into the left channel's result is the same number an interleaved scan would give.
    result.peak = std::max(simd::peak_abs(result.left), simd::peak_abs(result.right));
    result.sample_rate = rate;
    return result;
}

} // namespace ts
