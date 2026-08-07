#include "tabulasonora/envelope_machine.hpp"
#include "tabulasonora/insertion_effect.hpp"
#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/pitch_chain.hpp"
#include "tabulasonora/render_options.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/rom_locator.hpp"
#include "tabulasonora/soundfont_bank.hpp"
#include "tabulasonora/soundfont_sflist.hpp"
#include "tabulasonora/soundfont_writer.hpp"
#include "tabulasonora/send_effects.hpp"
#include "tabulasonora/sequence.hpp"
#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/smf_reader.hpp"
#include "tabulasonora/table_manifest.hpp"
#include "tabulasonora/tone_generator.hpp"
#include "tabulasonora/wav_writer.hpp"
#include "tabulasonora/wave_rom.hpp"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

/// Groups a byte count with thousands separators.
[[nodiscard]] std::string with_separators(std::int64_t value)
{
    std::string digits = std::to_string(value);
    for (std::size_t i = digits.size(); i > 3;) {
        i -= 3;
        digits.insert(i, ",");
    }
    return digits;
}

/// Reports what the embedded offset map says the engine expects.
///
/// This is `info` without a DLL: it answers "which file do I need?" before you have one, which is
/// the question somebody arriving at this repository actually has.
int manifest_command()
{
    const ts::TableManifest& manifest = ts::TableManifest::defaults();
    const ts::DllIdentity& dll = manifest.dll();

    std::cout << "expects   " << dll.product << ' ' << dll.version << " (" << dll.file_name << ")\n"
              << "size      " << with_separators(dll.size) << " bytes\n"
              << "sha256    " << dll.sha256 << '\n'
              << "timestamp " << dll.pe_timestamp << " (PE TimeDateStamp)\n"
              << "tables    " << manifest.cached_tables().size() << " static, "
              << manifest.live_regions().size() << " live regions\n";
    return 0;
}

/// Verifies a DLL and reports what the engine can see in it.
int info_command(const std::string& path)
{
    const ts::RomImage rom = ts::RomImage::open(path, ts::RomVerification::full);
    const ts::WaveRom waves{rom};

    std::cout << "verified  " << fs::path{path}.filename().string() << " against the pinned build\n"
              << "size      " << with_separators(rom.length()) << " bytes\n"
              << "sha256    " << rom.compute_sha256() << '\n'
              << "timestamp " << rom.read_pe_timestamp() << '\n'
              << '\n';

    std::int64_t table_bytes = 0;
    for (const ts::TableEntry& entry : rom.manifest().cached_tables()) {
        table_bytes += entry.size;
    }
    std::cout << "tables    " << rom.manifest().cached_tables().size() << " ("
              << with_separators(table_bytes) << " bytes)\n";

    for (int bank = 0; bank < 2; ++bank) {
        const int regions = ts::WaveRom::region_count(bank);
        std::cout << "wave ROM  bank " << static_cast<char>('A' + bank) << ": " << regions
                  << " regions from 0x" << std::hex << waves.bank_base(bank) << std::dec << " ("
                  << with_separators(static_cast<std::int64_t>(regions) * ts::WaveRom::region_size)
                  << " bytes)\n";
    }
    return 0;
}

/// Writes every static table out as a byte-for-byte `.bin` slice.
///
/// Reads the DLL, never runs it. This is the whole extraction path: a slice at a manifest offset.
int extract_tables_command(const std::string& path, const fs::path& output)
{
    const ts::RomImage rom = ts::RomImage::open(path, ts::RomVerification::full);
    fs::create_directories(output);

    std::size_t written = 0;
    std::int64_t bytes = 0;

    for (const ts::TableEntry& entry : rom.manifest().cached_tables()) {
        const std::vector<std::uint8_t> data = rom.read(entry);

        std::ofstream stream{output / entry.name, std::ios::binary};
        if (!stream) {
            throw std::runtime_error("Cannot write '" + (output / entry.name).string() + "'.");
        }
        stream.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        if (!stream) {
            throw std::runtime_error("Short write to '" + (output / entry.name).string() + "'.");
        }

        ++written;
        bytes += static_cast<std::int64_t>(data.size());
    }

    std::cout << "Verified " << fs::path{path}.filename().string() << " against the pinned build.\n"
              << "Wrote " << written << " tables (" << with_separators(bytes) << " bytes) to "
              << output.string() << '\n';
    return 0;
}

/// Renders one note and writes interleaved raw float32 stereo.
///
/// The fastest way to A/B a single patch against the reference build: same arguments, same output
/// format, so the two files diff directly.
void write_interleaved(const fs::path& output,
                       std::span<const float> left,
                       std::span<const float> right)
{
    std::ofstream stream{output, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot write '" + output.string() + "'.");
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const float frame[2]{left[i], right[i]};
        stream.write(reinterpret_cast<const char*>(frame), sizeof(frame));
    }
    if (!stream) {
        throw std::runtime_error("Short write to '" + output.string() + "'.");
    }
}

/// Renders one note through the block loop, the way the oracle gate does.
///
/// **This is the path to compare against `notebatch`'s output**, and the reason it exists is that
/// the per-note renderer below is not. That one takes the ideal `pow(2, x/12000)` for its playback
/// rate; every voice in the block loop goes through `g_ramp_exp_tbl`, which is not a true
/// exponential and sits up to 4.66 cents flat of one across an octave. A single-note render taken
/// from the retired path and held against an oracle case therefore disagrees by a few cents for
/// reasons that have nothing to do with what is being investigated -- which has already cost this
/// project an afternoon once.
///
/// The setup mirrors `note_oracle_tests.cpp` exactly, down to the eight discarded 512-frame blocks
/// the harness runs after its reset and the module's own event staging, so the two renders are
/// comparable sample for sample rather than approximately.
int render_note_block_loop(const std::string& path,
                           int program,
                           int note,
                           int velocity,
                           double hold_seconds,
                           double tail_seconds,
                           int channel,
                           const fs::path& output,
                           int map)
{
    const ts::RomImage rom = ts::RomImage::open(path, ts::RomVerification::quick);
    ts::NoteRenderer notes{rom};

    ts::ToneGeneratorOptions options;
    options.ports = 1;
    options.map = static_cast<ts::ToneMap>(map);
    options.event_delay_blocks = 4;
    options.bypass_output_filter = false;
    ts::ToneGenerator generator{notes, options};

    std::vector<float> discard_left(512);
    std::vector<float> discard_right(512);
    for (int i = 0; i < 8; ++i) {
        generator.render(discard_left, discard_right);
    }

    generator.send_channel(0xB0 | channel, 0, 0);
    generator.send_channel(0xB0 | channel, 32, 0);
    generator.send_channel(0xB0 | channel, 7, 127);
    generator.send_channel(0xB0 | channel, 10, 64);
    generator.send_channel(0xB0 | channel, 91, 0);
    generator.send_channel(0xB0 | channel, 93, 0);
    generator.send_channel(0xC0 | channel, program, 0);

    const auto rate = static_cast<double>(ts::ToneGenerator::sample_rate);
    const auto total = static_cast<std::size_t>((hold_seconds + tail_seconds) * rate);
    const auto off_at = static_cast<std::size_t>(hold_seconds * rate);

    std::vector<float> left(total);
    std::vector<float> right(total);

    generator.send_channel(0x90 | channel, note, velocity);
    std::size_t position = 0;
    bool released = false;
    while (position < total) {
        if (!released && position >= off_at) {
            generator.send_channel(0x80 | channel, note, 0);
            released = true;
        }
        const std::size_t count =
            std::min<std::size_t>(ts::NoteRenderer::control_block, total - position);
        generator.render(std::span{left}.subspan(position, count),
                         std::span{right}.subspan(position, count));
        position += count;
    }

    write_interleaved(output, left, right);
    std::cout << "block loop: " << left.size() << " frames\n";
    return 0;
}

int render_note_command(const std::string& path,
                        int program,
                        int note,
                        int velocity,
                        double hold_seconds,
                        const fs::path& output,
                        int map)
{
    const ts::RomImage rom = ts::RomImage::open(path, ts::RomVerification::quick);
    ts::NoteRenderer renderer{rom};

    const ts::RenderedNote voice = renderer.render_note(
        program, note, velocity, hold_seconds, /*tail_seconds=*/1.8, static_cast<ts::ToneMap>(map));

    write_interleaved(output, voice.left, voice.right);
    std::cout << voice.name << ": " << voice.left.size() << " frames\n";
    return 0;
}

/// The module's pitch ramp word for a pitch in milli-semitones.
///
/// `voice_pitch_block_init @ 180082e10` is the only place a pitch becomes a playback rate, and it
/// spells the conversion out: `0x38000 + ((x & 0x7fffff) >> 22) + (x << 9) / 0x177 + ((x << 9) >>
/// 31)`. The division is C truncation and the last term is the toward-zero correction, so the two
/// have to be written separately rather than folded into one floor. `512 / 375` is `16384 / 12000`
/// — one unit is a 16,384th of an octave.
int pitch_ramp_word(int milli_semitones)
{
    const auto shifted = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(milli_semitones) << 9U);
    //
    // Masked even, because `voice_pitch_block_init` ends `*(uint *)(param_1 + 0xb8) = uVar4 &
    // 0xfffffffe` and `PitchRamp` does the same. Without it this diagnostic prints a number one
    // above what the module holds and what this engine actually renders with, which reads as a
    // discrepancy when there is none -- it cost an afternoon once.
    return (((milli_semitones & 0x7FFFFF) >> 22) + 0x38000 + (shifted / 0x177)
            + (shifted < 0 ? -1 : 0))
           & ~1;
}

/// Prints the pitch chain for one note, term by term, without rendering anything.
///
/// The instrument for comparing this engine's tuning against the module's *exactly*. Estimating a
/// fundamental from rendered audio resolves a few cents at best — enough to have found a 320
/// milli-semitone error, nowhere near enough to chase what is left.
///
/// `pitch_word` is the half that closes it. `scdec postrace` with `TS_POSTRACE_BLOCK=32` dumps
/// `g_voice_ramp_pitch` (@`181a1cbf0`, stride 0x18: +8 current, +0xc target, +0x14 the cached 16.16
/// increment), and once a note has settled `cur == tgt` **is the module's pitch as an integer**.
/// Printing ours in the same word makes the comparison integer against integer, with no audio, no
/// fundamental estimate and no exponential in between: one unit is 375/512 of a milli-semitone.
/// A voice under vibrato never settles, so only cases whose word holds still are comparable.
///
/// The terms are printed separately because a disagreement is only useful once it is attributed:
/// the module's own chain (`partial_compute_pitch` @ `18005fc20`) is `native = root×1000 − fine +
/// 0x400` against `base = key×1000 + weight + key-follow curve + coarse`, and each of those is a
/// place this port could differ.
int pitch_command(const std::string& path,
                  int program,
                  int note,
                  int velocity,
                  int map,
                  int bank)
{
    const ts::RomImage rom = ts::RomImage::open(path, ts::RomVerification::quick);
    ts::NoteRenderer renderer{rom};
    const ts::PatchDirectory& directory = renderer.directory();
    const ts::EnvelopeMachine envelope{renderer.tables()};
    const ts::PitchChain pitch{renderer.tables(), envelope};

    const auto tone_map = static_cast<ts::ToneMap>(map);
    const int tone_number = directory.program_to_tone(program, tone_map, bank);
    const ts::ResolvedTone resolved = directory.resolve(tone_number, note, velocity);

    std::cout << "program " << program << " note " << note << " velocity " << velocity
              << " map " << map << " bank " << bank << " -> tone " << tone_number << " \""
              << resolved.name << "\"\n";

    for (const ts::ResolvedPartial& sounding : resolved.partials) {
        const ts::PartialParameters partial =
            directory.partial_by_slot(tone_number, sounding.partial_index);
        const ts::WaveDescriptor& descriptor = sounding.descriptor;

        const int key_center = partial.key_center();
        const int base = pitch.base_pitch_milli_semitones(partial, note, key_center);
        const ts::PitchChain::KeyFollow follow =
            ts::PitchChain::key_follow_key(partial, note, key_center);
        const int follow_key = std::clamp(follow.key, 0, 0x7F);
        const auto curve = renderer.tables().kf_pitch();

        // The settled pitch envelope, which a traced increment carries and `base` does not. A
        // patch whose envelope sustains away from zero reads as a tuning error against the module
        // unless this is added in, and it sustains steadily, so no steadiness filter catches it.
        const std::optional<ts::PitchEnvelope> pitch_envelope =
            pitch.envelope_offsets(partial, note, velocity);
        const int sustain = pitch_envelope ? pitch_envelope->targets.back() : 0;
        const double native = descriptor.native_milli_semitones();
        const double first_only =
            (descriptor.root_key * 1000.0) + 1024.0 - descriptor.fine_tune;
        const double ratio = std::pow(2.0, (base - native) / 12000.0);
        const double ratio_first = std::pow(2.0, (base - first_only) / 12000.0);

        std::cout << "  partial " << sounding.partial_index << ": wave " << sounding.wave
                  << ", loop " << descriptor.loop << '\n'
                  << "    root_key      " << descriptor.root_key << '\n'
                  << "    fine_tune     " << descriptor.fine_tune << '\n'
                  << "    second_fine   " << descriptor.second_fine_tune << '\n'
                  << "    key_center    " << key_center << '\n'
                  << "    coarse tune   " << partial.coarse_tune_milli_semitones() << '\n'
                  << "    kf byte 0x13  " << partial.pitch_key_follow() << " (follow amount)\n"
                  << "    kf byte 0x17  " << static_cast<int>(partial.raw()[0x17])
                  << " (the row, when the part's +0x10 is not zero)\n"
                  // Both candidates for the random-pitch depth, printed together because which one
                  // it is remains open: `partial_compute_pitch` reads +0x12, this port reads 0x1A,
                  // and the note in PitchChain::create records why the byte has not been switched.
                  // A partial where the two disagree is what decides it.
                  << "    hdr row 0x17  " << partial.pitch_curve_row()
                  << "  (the tone header's byte, which selects the curve row)\n"
                  << "    jitter 0x12   " << static_cast<int>(partial.raw()[0x12])
                  << "  (the engine's byte)\n"
                  << "    jitter 0x1A   " << static_cast<int>(partial.raw()[0x1A])
                  << "  (the byte this port reads)\n"
                  << "    follow key    " << follow_key << '\n'
                  << "    follow weight " << follow.weight << '\n'
                  // All four rows, though only row 2 is used: printing the alternatives is what
                  // settled which one the module reads, and is what would settle it again.
                  << "    curve row 0   " << curve[static_cast<std::size_t>(follow_key)] << '\n'
                  << "    curve row 1   " << curve[static_cast<std::size_t>(0x80 + follow_key)]
                  << '\n'
                  << "    curve row 2   " << curve[static_cast<std::size_t>(0x100 + follow_key)]
                  << "   <- the one applied\n"
                  << "    curve row 3   " << curve[static_cast<std::size_t>(0x180 + follow_key)]
                  << '\n'
                  << "    base_pitch    " << base << '\n'
                  << "    pitch_sustain " << sustain << '\n'
                  << "    base_settled  " << (base + sustain) << '\n'
                  << "    native        " << native << "   (voice+0x200, both fine tunes)\n"
                  << "    native_1fc    " << first_only << "   (voice+0x1fc, first fine only)\n"
                  << "    ratio         " << std::setprecision(10) << ratio << '\n'
                  << "    ratio_1fc     " << ratio_first << std::setprecision(6) << '\n'
                  << "    pitch_word    "
                  << pitch_ramp_word(static_cast<int>(base + sustain - native))
                  << "   (g_voice_ramp_pitch cur/tgt; 1 unit = 375/512 mst)\n";
    }
    return 0;
}

/// Renders one send effect's impulse response to raw interleaved float32.
///
/// The A/B harness for the effect networks: an impulse in, the wet tail out, so the two builds diff
/// directly. Twelve of these comparisons were once green upstream while testing nothing at all --
/// the fixture windows were shorter than the delays, so both sides were silent and agreed
/// perfectly. Ask for enough samples to hear something.
int dump_effect_command(
    const std::string& dll, const std::string& kind, int type, int samples, const fs::path& output)
{
    // The coefficients come out of the DLL, so this needs it open even though no note sounds.
    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    ts::EffectPresets::ensure_from(rom);

    std::vector<float> input(static_cast<std::size_t>(samples), 0.0F);
    if (!input.empty()) {
        input[0] = 1.0F;
    }
    std::vector<float> left(input.size(), 0.0F);
    std::vector<float> right(input.size(), 0.0F);

    if (kind == "reverb") {
        ts::Reverb::for_type(type).process(input, left, right);
    } else if (kind == "chorus") {
        ts::Chorus::for_type(type).process(input, left, right);
    } else if (kind == "delay") {
        ts::SystemDelay::for_type(type).process(input, left, right);
    } else {
        throw std::runtime_error("Unknown effect '" + kind
                                 + "'; expected reverb, chorus or delay.");
    }

    std::ofstream stream{output, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot write '" + output.string() + "'.");
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const float frame[2]{left[i], right[i]};
        stream.write(reinterpret_cast<const char*>(frame), sizeof(frame));
    }

    std::cout << "wrote " << output.string() << ": " << samples << " stereo samples\n";
    return 0;
}

/// Renders a Standard MIDI File to a WAV.
///
/// `--stream` drives the real-time block loop instead of rendering note by note. The two share
/// their DSP, so what the flag exposes is the difference the architecture makes: a 64-voice limit
/// that actually steals, live controllers, and effect types that can change mid-song.
int render_command(int chorus_phase,
                   const std::string& dll,
                   const fs::path& midi,
                   const fs::path& output,
                   int map,
                   const ts::RenderOptions& base,
                   bool hardware_polyphony,
                   int polyphony,
                   int ports,
                   int loops,
                   double fade_seconds)
{
    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    ts::NoteRenderer notes{rom};

    ts::RenderOptions options = base;
    options.map = static_cast<ts::ToneMap>(map);

    // The block loop is the renderer. `--stream` now means "with the hardware's voice limit", and
    // an explicit --polyphony beats both.
    if (hardware_polyphony && polyphony == ts::ToneGeneratorOptions::unlimited_polyphony) {
        polyphony = ts::VoicePool::default_polyphony;
    }

    ts::RenderResult result;
    int stole = 0;
    int peak_voices = 0;
    bool limited = false;
    {
        ts::ToneGeneratorOptions engine_options;
        engine_options.ports = ports;
        engine_options.map = options.map;
        engine_options.drum_channel = options.drum_channel;
        engine_options.reverb = options.reverb;
        engine_options.chorus = options.chorus;
        engine_options.delay = options.delay;
        engine_options.efx = options.efx;
        engine_options.reverb_type = options.reverb_type;
        engine_options.chorus_type = options.chorus_type;
        engine_options.delay_type = options.delay_type;
        engine_options.output_gain = options.output_gain;
        engine_options.polyphony = polyphony;
        engine_options.channels = options.channels;

        ts::ToneGenerator generator{notes, engine_options};
        if (chorus_phase != 0) {
            generator.seed_chorus_phase(chorus_phase);
        }
        // Not an engine option: the row is settable while the engine runs, because it is what the
        // module would have taken from a bank select.
        generator.set_drum_map_row(options.drum_map_row);

        ts::SequencePlayer player = ts::SequencePlayer::from_file(generator, midi);
        if (loops >= 2 && player.loop()) {
            player.set_loop_count(loops);
            player.set_fade_seconds(fade_seconds);
        } else if (loops >= 2) {
            std::cout << "no loop points in this file; rendering it once\n";
        }
        result = player.render_to_end(options.tail_seconds, options.end_seconds);
        stole = generator.stolen_voices();
        peak_voices = generator.voice_slots();
        limited = generator.polyphony_limit_reached();
    }
    ts::wav::write(output, result.left, result.right, result.sample_rate);

    const double seconds = static_cast<double>(result.left.size()) / result.sample_rate;
    std::cout << midi.filename().string() << ": " << result.note_count << " notes, " << std::fixed
              << std::setprecision(2) << seconds << " s, peak " << std::setprecision(6)
              << result.peak << "\n";

    // Whether the polyphony setting mattered. A render that never ran out sounds the same at every
    // limit, so a digest taken from it says nothing about the others -- and one that did steal is a
    // digest that is only about the limit it was taken at.
    std::cout << "  polyphony: " << peak_voices << " slots";
    if (stole > 0) {
        std::cout << ", " << stole << " voices stolen -- a higher limit renders this differently\n";
    } else if (limited) {
        std::cout << ", grown on demand -- a fixed limit below this would steal\n";
    } else {
        std::cout << ", never exhausted -- identical at any higher limit\n";
    }

    std::cout << "wrote " << output.string() << "\n";
    return 0;
}

/// A deterministic broadband signal for the effect stages.
///
/// Not an impulse. A decaying impulse response falls into denormal range within a second or two,
/// where the arithmetic is an order of magnitude slower, and the stage would then be measuring the
/// denormal penalty rather than the algorithm. Keeping the rings excited keeps every sample normal.
[[nodiscard]] std::vector<float> excitation(std::size_t count)
{
    std::vector<float> samples(count);
    std::uint32_t state = 0x2545F491U;
    for (float& sample : samples) {
        // xorshift32, so the signal is identical on every machine and every run.
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        sample = (static_cast<float>(state >> 8) / static_cast<float>(0x800000)) - 1.0F;
    }
    return samples;
}

/// Times one stage and prints its row.
///
/// Best-of-N rather than the mean. Measurement noise is one-sided -- a scheduling preemption can
/// only make an iteration slower -- so the minimum is the closest estimate of the work actually
/// being done, and it is far more stable run to run than an average.
void report(const std::string& name,
            double audio_seconds,
            int iterations,
            const std::function<void()>& stage)
{
    // One untimed pass, so first-touch page faults and cache warming are not attributed to the
    // work.
    stage();

    double best = std::numeric_limits<double>::max();
    for (int i = 0; i < iterations; ++i) {
        const auto started = std::chrono::steady_clock::now();
        stage();
        const std::chrono::duration<double, std::milli> elapsed =
            std::chrono::steady_clock::now() - started;
        best = std::min(best, elapsed.count());
    }

    std::ostringstream row;
    row << std::left << std::setw(24) << name << std::right << std::fixed << std::setw(9)
        << std::setprecision(1) << best << std::setw(9) << std::setprecision(1)
        << (audio_seconds * 1000.0 / best);
    std::cout << row.str() << '\n';
}

/// Times the render path stage by stage, so a performance change can be attributed rather than
/// guessed at.
///
/// Stages are reported separately because they move independently: the effects are single-sample
/// feedback loops, the block loop is dominated by the sampler and filter, and the offline renderer
/// spends much of its time in elementwise mix loops that neither of the others runs.
///
/// There is no allocation column, unlike the reference build's. It had one because a managed
/// renderer's allocation rate is a real cost the timing hides; here the per-note allocations are
/// visible in the source and the pools that avoid them are explicit.
int bench_command(const std::string& dll, const fs::path& midi, int iterations)
{
    iterations = std::max(1, iterations);

    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    ts::NoteRenderer notes{rom};

    std::cout << std::thread::hardware_concurrency() << " cores, scalar kernels, best of "
              << iterations << "\n\n"
              << "stage                          ms      xrt\n"
              << "-------------------------------------------\n";

    // Effects first: they need no MIDI and no wave data, so they always report.
    constexpr double effect_seconds = 10.0;
    const auto effect_samples =
        static_cast<std::size_t>(effect_seconds * ts::NoteRenderer::sample_rate);
    const std::vector<float> input = excitation(effect_samples);
    std::vector<float> wet_left(effect_samples);
    std::vector<float> wet_right(effect_samples);

    ts::Reverb reverb = ts::Reverb::for_type(std::nullopt);
    report("reverb", effect_seconds, iterations, [&] {
        reverb.reset();
        reverb.process(input, wet_left, wet_right);
    });

    ts::Chorus chorus = ts::Chorus::for_type(std::nullopt);
    report("chorus", effect_seconds, iterations, [&] {
        chorus.reset();
        chorus.process(input, wet_left, wet_right);
    });

    ts::SystemDelay delay = ts::SystemDelay::for_type(0);
    report("delay", effect_seconds, iterations, [&] {
        delay.reset();
        delay.process(input, wet_left, wet_right);
    });

    // One voice, which isolates the sampler, the filter and the envelopes from the mix.
    constexpr double note_hold_seconds = 1.0;
    report("note (piano, 1s hold)", note_hold_seconds + 1.8, iterations, [&] {
        (void)notes.render_note(/*program=*/0,
                                /*note=*/60,
                                /*velocity=*/100,
                                note_hold_seconds,
                                /*tail_seconds=*/1.8);
    });

    if (midi.empty()) {
        std::cout << "\nPass a MIDI file to also time the offline renderer and the block loop.\n";
        return 0;
    }

    // Parsed once: what is being timed is rendering, not the SMF reader.
    const ts::Sequence sequence =
        ts::sequence_builder::build(ts::smf::read(midi, ts::NoteRenderer::sample_rate));
    const double seconds =
        static_cast<double>(sequence.last_event_position) / ts::NoteRenderer::sample_rate;

    report("block loop (64 voices)", seconds, iterations, [&] {
        ts::ToneGenerator generator{notes};
        ts::SequencePlayer player = ts::SequencePlayer::from_file(generator, midi);
        (void)player.render_to_end(/*tail_seconds=*/2.2);
    });

    std::cout << '\n'
              << midi.filename().string() << ": " << std::fixed << std::setprecision(1) << seconds
              << " s, " << sequence.notes.size() << " notes\n";
    return 0;
}

/// Applies a `1,2,5` channel list to a mask, as a mixer labels channels.
void apply_channels(ts::ChannelMask& mask, const std::vector<std::string>& lists, bool mute)
{
    for (const std::string& list : lists) {
        std::istringstream stream{list};
        std::string item;
        while (std::getline(stream, item, ',')) {
            if (item.empty()) {
                continue;
            }
            const int channel = std::stoi(item) - 1;
            if (channel < 0 || channel >= ts::ChannelMask::channel_count) {
                throw std::runtime_error("Channel '" + item + "' is outside 1-"
                                         + std::to_string(ts::ChannelMask::channel_count) + ".");
            }
            if (mute) {
                mask.set_muted(channel, true);
            } else {
                mask.set_soloed(channel, true);
            }
        }
    }
}

/// Tags each file with the insertion-EFX traffic it carries, one JSON row per file that has any.
///
/// Corpus triage for the EFX block: which GS types the wild actually selects, and which files
/// route notes through the block heavily enough to hear. The files go through `smf::read` — the
/// same reader the render path uses, so XMI, MUS and the rest are scanned as they would be
/// played — and the SysEx walk mirrors `ToneGenerator::send_sysex` byte for byte: DT1 only, the
/// checksum folded to zero, `50` accepted beside `40`, and a message spanning several `40 03`
/// addresses walked one byte at a time.
///
/// Names and the implemented flag come from the DLL's own directory, which is why this needs the
/// ROM at all; `tools/scan_midi_efx.py` drives it over an archive and aggregates the rows.
int scan_efx_command(const std::string& dll, const std::vector<fs::path>& files)
{
    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    ts::InsertionEffect efx{rom};

    // The directory with each type selected once, so a row can say whether this engine renders
    // the type or passes it through. Keyed by the GS type key, `(MSB << 8) | LSB`.
    //
    // The implemented flag is *derived*, not a list kept here: `InsertionEffect::implemented()`
    // reports whether the selected type resolved to a real processor rather than the passthrough,
    // so transcribing an algorithm moves these rows on the next scan with no edit to this file.
    // Do not replace it with a hard-coded set -- a stale one would quietly recommend the wrong
    // comparison material.
    std::map<int, std::pair<std::string, bool>> directory;
    for (const ts::EfxRecord& record : efx.directory()) {
        if (record.type_key == 0xFFFF) {
            continue;
        }
        efx.select_type(record.type_key >> 8, record.type_key & 0xFF);
        directory[record.type_key] = {record.name, efx.implemented()};
    }

    const auto key_label = [](int key) {
        std::ostringstream text;
        text << std::uppercase << std::setfill('0') << std::hex << std::setw(2)
             << ((key >> 8) & 0x7F) << ' ' << std::setw(2) << (key & 0x7F);
        return text.str();
    };

    for (const fs::path& file : files) {
        std::vector<ts::MidiEvent> events;
        try {
            events = ts::smf::read(file);
        } catch (const std::exception& error) {
            std::cerr << file.string() << ": " << error.what() << '\n';
            continue;
        }

        // Per-type tallies. Key -1 is the power-on state: parts routed through the block while
        // no type was ever selected, which sounds as Thru with the default sends.
        struct TypeUse {
            int selects = 0;
            std::int64_t notes = 0;
        };
        std::map<int, TypeUse> uses;

        int selected = -1;
        int type_msb = 0;
        std::array<bool, 64> enabled{};
        std::set<int> parts_on;
        int switch_writes = 0;
        int param_writes = 0;
        int send_writes = 0;
        int control_writes = 0;
        std::int64_t notes_through = 0;
        std::optional<std::int64_t> first_write;

        // The resets that return the block to power-on: GM/GM2 System On, XG System On, GS
        // reset and the system mode set, exactly the messages that reach `stream_reset`.
        const auto reset_state = [&] {
            selected = -1;
            type_msb = 0;
            enabled.fill(false);
        };

        for (const ts::MidiEvent& event : events) {
            if (event.kind == ts::MidiEventKind::channel) {
                if (event.message_type() == 0x90 && event.data2 > 0) {
                    const int part = (event.port & 3) * 16 + event.channel();
                    if (enabled[static_cast<std::size_t>(part)]) {
                        ++notes_through;
                        ++uses[selected].notes;
                    }
                }
                continue;
            }

            const std::vector<std::uint8_t>& bytes = event.sysex;
            if (bytes.size() >= 6 && bytes[0] == 0xF0 && bytes[1] == 0x7E && bytes[3] == 0x09
                && (bytes[4] == 0x01 || bytes[4] == 0x03)) {
                reset_state();
                continue;
            }
            if (bytes.size() >= 9 && bytes[0] == 0xF0 && bytes[1] == 0x43 && bytes[3] == 0x4C
                && bytes[4] == 0x00 && bytes[5] == 0x00 && bytes[6] == 0x7E) {
                reset_state();
                continue;
            }
            if (bytes.size() < 11 || bytes[0] != 0xF0 || bytes[1] != 0x41 || bytes[3] != 0x42
                || bytes[4] != 0x12) {
                continue;
            }
            unsigned int sum = 0;
            for (std::size_t i = 5; i + 1 < bytes.size(); ++i) {
                sum += bytes[i];
            }
            if (sum % 0x80 != 0) {
                continue;
            }

            const int a1 = bytes[5];
            const int a2 = bytes[6];
            const int a3 = bytes[7];
            const std::span<const std::uint8_t> data{bytes.data() + 8, bytes.size() - 10};
            if (data.empty()) {
                continue;
            }

            if ((a1 == 0x00 && a2 == 0x00 && a3 == 0x7F)
                || ((a1 == 0x40 || a1 == 0x50) && a2 == 0x00 && a3 == 0x7F)) {
                reset_state();
                continue;
            }
            if (a1 != 0x40 && a1 != 0x50) {
                continue;
            }
            const int block_port = a1 == 0x50 ? 1 : (event.port & 3);

            if (a2 == 0x03) {
                if (!first_write) {
                    first_write = event.position;
                }
                int address = a3;
                for (const std::uint8_t byte : data) {
                    if (address == 0x00) {
                        type_msb = byte;
                    } else if (address == 0x01) {
                        selected = (type_msb << 8) | byte;
                        ++uses[selected].selects;
                    } else if (address >= 0x03 && address <= 0x16) {
                        ++param_writes;
                    } else if (address >= 0x17 && address <= 0x1A) {
                        // Sends *and* the routing byte at 1A. These are the ranges
                        // `InsertionEffect::set_parameter` folds into its own parameter array --
                        // 03-16 to index 0, 17-1A to 0x14, 1B-1E to 0x1C -- and they have to stay
                        // in step with it, or a write the engine consumes is counted as nothing
                        // here. 1A was missing and is why this range now ends there.
                        ++send_writes;
                    } else if (address >= 0x1B && address <= 0x1E) {
                        ++control_writes;
                    }
                    ++address;
                }
            } else if ((a2 & 0xF0) == 0x40 && a3 == 0x22) {
                if (!first_write) {
                    first_write = event.position;
                }
                const int part =
                    block_port * 16 + ts::sequence_builder::channel_from_block(a2 & 0x0F);
                ++switch_writes;
                enabled[static_cast<std::size_t>(part)] = data[0] != 0;
                if (data[0] != 0) {
                    parts_on.insert(part + 1); // as a mixer labels them, like --mute
                }
            }
        }

        if (uses.empty() && switch_writes == 0 && param_writes == 0 && send_writes == 0
            && control_writes == 0) {
            continue;
        }

        nlohmann::json types = nlohmann::json::object();
        for (const auto& [key, use] : uses) {
            nlohmann::json entry{{"selects", use.selects}, {"notes", use.notes}};
            if (key < 0) {
                entry["name"] = "(power-on Thru)";
                entry["implemented"] = true;
            } else if (const auto found = directory.find(key); found != directory.end()) {
                entry["name"] = found->second.first;
                entry["implemented"] = found->second.second;
            } else {
                entry["name"] = "(unknown type)";
                entry["implemented"] = false;
            }
            types[key < 0 ? std::string{"--"} : key_label(key)] = std::move(entry);
        }

        const std::int64_t last = events.empty() ? 0 : events.back().position;
        nlohmann::json row{{"path", file.string()},
                           {"size", static_cast<std::int64_t>(fs::file_size(file))},
                           {"seconds", std::round(static_cast<double>(last) / 3200.0) / 10.0},
                           {"types", std::move(types)},
                           {"param_writes", param_writes},
                           {"send_writes", send_writes},
                           {"control_writes", control_writes},
                           {"switch_writes", switch_writes},
                           {"parts_on", parts_on},
                           {"notes_through", notes_through}};
        if (first_write) {
            row["first_write_s"] =
                std::round(static_cast<double>(*first_write) / 3200.0) / 10.0;
        }
        std::cout << row.dump() << '\n';
    }
    return 0;
}

} // namespace

/// Exports the whole sound set as a SoundFont, plus the five `.sflist.json` maps beside it.
int export_soundfont_command(const std::string& dll,
                             const fs::path& output,
                             ts::sf2::Codec codec,
                             bool write_maps,
                             bool gs_modulators)
{
    if (!ts::sf2::codec_available(codec)) {
        throw std::runtime_error(
            "This build has no encoder for that codec; configure with libFLAC or libvorbis, or "
            "use --codec pcm.");
    }

    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    const ts::TableSet tables = ts::TableSet::from_rom(rom);
    const ts::PatchDirectory directory{tables};
    const ts::DrumKitTable kits{rom};
    const ts::WaveRom wave_rom{rom};
    const ts::Interpolator interpolator{tables};
    const ts::EnvelopeMachine machine{tables};
    const ts::TvaChain levels{tables, machine};
    const ts::TvfChain filters{tables, machine};
    const ts::PitchChain pitches{tables, machine};
    const ts::LfoEngine lfos{tables};
    ts::Sampler sampler{wave_rom, interpolator};

    std::cout << "Collecting waves...\n";
    const std::vector<int> waves = ts::sf2::SampleSet::census(directory, kits);
    const ts::sf2::SampleSet set = ts::sf2::SampleSet::build(directory, sampler, waves);
    std::cout << "  " << waves.size() << " referenced, " << set.runs().size() << " runs after "
              << set.shared_count() << " shared\n";

    ts::sf2::BankOptions options;
    options.software = "tabula-sonora";
    options.comment = "Exported from the Sound Canvas VA wave ROM.";
    options.gs_modulators = gs_modulators;

    std::cout << "Building the bank...\n";
    const ts::sf2::BankBuild built =
        ts::sf2::build_bank(directory, kits, set, levels, filters, pitches, lfos, options);
    std::cout << "  " << built.melodic_presets << " melodic + " << built.drum_presets
              << " drum presets, " << built.bank.instruments.size() << " instruments\n";
    std::cout << "  amplitude envelope fit: mean rms " << built.mean_fit_error() << " of peak, "
              << built.overflowed_zones << " of " << built.fitted_zones
              << " zones needing more segments than SF2 has\n";

    std::cout << "  per-instrument modulators: " << built.half_damper_instruments
              << " half-damper, " << built.env_modifier_partials
              << " filter-envelope modify, " << built.inverted_velocity_partials
              << " inverted velocity\n";

    std::cout << "Writing " << output << "...\n";
    const ts::sf2::WriteReport report = ts::sf2::write(output, built.bank, codec);
    std::cout << "  igen " << report.igen_count << ", pgen " << report.pgen_count << "; xdta is "
              << (report.needed_xdta() ? "load-bearing" : "not needed at these counts") << "\n";
    std::cout << "  sample data " << (static_cast<double>(report.pcm_bytes) / 1048576.0)
              << " MB, file " << (static_cast<double>(report.file_bytes) / 1048576.0) << " MB\n";

    if (!write_maps) {
        return 0;
    }

    ts::sf2::SflistOptions sflist;
    sflist.file_name = output.filename().string();
    for (const auto& [name, selector] : ts::tone_map_choices()) {
        ts::sf2::SflistReport sflist_report;
        const std::string text = ts::sf2::build_sflist(
            directory, kits, static_cast<ts::ToneMap>(selector), sflist, sflist_report);

        fs::path path = output;
        path.replace_extension();
        path += "." + name + ".sflist.json";

        std::ofstream file{path, std::ios::binary};
        if (!file) {
            throw std::runtime_error("Cannot write '" + path.string() + "'.");
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));

        std::cout << "  " << path.filename().string() << ": " << sflist_report.melodic_mappings
                  << " melodic (" << sflist_report.fallback_mappings << " capital-tone fallback), "
                  << sflist_report.drum_mappings << " drum\n";
    }

    return 0;
}

int main(int argc, char** argv)
{
    CLI::App app{"A native implementation of the Roland Sound Canvas VA synth voice.",
                 "tabula-sonora"};
    app.require_subcommand(1);
    app.footer("The ROM is found from --dll, then $TS_SCCORE_DLL, then ./SCCore.dll.");

    CLI::App* manifest = app.add_subcommand("manifest", "Show the pinned DLL build and table map.");

    // One option, added to every subcommand that needs a ROM. It is not a positional because it
    // usually is not typed at all: TS_SCCORE_DLL pins it for the shell, and a command then names
    // only its actual subject.
    std::string dll_path;
    const auto add_dll = [&dll_path](CLI::App* command) {
        command->add_option("--dll", dll_path, "Path to SCCore.dll; overrides TS_SCCORE_DLL");
    };

    CLI::App* info = app.add_subcommand("info", "Verify an SCCore.dll and describe it.");
    add_dll(info);

    fs::path output_directory;
    CLI::App* extract =
        app.add_subcommand("extract-tables", "Write every static table out as a .bin slice.");
    add_dll(extract);
    extract->add_option("output", output_directory, "Directory to write into")->required();

    fs::path soundfont_output;
    ts::sf2::Codec soundfont_codec = ts::sf2::Codec::pcm;
    bool no_maps = false;
    bool no_gs_modulators = false;
    CLI::App* export_soundfont = app.add_subcommand(
        "export-soundfont", "Export the whole sound set as a SoundFont plus its five map files.");
    add_dll(export_soundfont);
    export_soundfont->add_option("output", soundfont_output, "Output .sf2 path")->required();
    export_soundfont
        ->add_option("--codec", soundfont_codec,
                     "Sample storage: pcm (exact, portable), flac (lossless, spessasynth only), "
                     "vorbis (lossy, portable)")
        ->transform(CLI::CheckedTransformer(
            std::vector<std::pair<std::string, ts::sf2::Codec>>{
                {"pcm", ts::sf2::Codec::pcm},
                {"flac", ts::sf2::Codec::flac},
                {"vorbis", ts::sf2::Codec::vorbis}},
            CLI::ignore_case));
    export_soundfont->add_flag("--no-maps", no_maps, "Write only the bank, not the .sflist.json maps");
    export_soundfont->add_flag("--no-gs-modulators", no_gs_modulators,
                               "Write only the reader's own default modulators into DMOD");


    int program = 0;
    int note = 60;
    int velocity = 100;
    double hold_seconds = 1.0;
    int map = 4;
    int lookup_bank = 0;
    fs::path output_file;
    CLI::App* render_note =
        app.add_subcommand("render-note", "Render one note to raw interleaved float32.");
    double note_tail_seconds = 1.8;
    int note_channel = 0;
    bool per_note_renderer = false;
    add_dll(render_note);
    render_note->add_option("program", program, "Program number, 0-127")->required();
    render_note->add_option("note", note, "MIDI note, 0-127")->required();
    render_note->add_option("velocity", velocity, "MIDI velocity, 1-127")->required();
    render_note->add_option("hold", hold_seconds, "Seconds from note-on to note-off")->required();
    render_note->add_option("output", output_file, "Output .f32 path")->required();
    render_note->add_option("map", map,
                   "Tone map: 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820, or xg to start in XG mode "
                   "(names accepted: sc55, sc88, sc88pro, sc8820, xg)")
        ->transform(CLI::CheckedTransformer(ts::tone_map_choices(), CLI::ignore_case));
    render_note->add_option("--tail", note_tail_seconds,
                            "Seconds rendered past the note-off; 1.8 matches the oracle sweep");
    render_note->add_option("--channel", note_channel,
                            "0-based channel; 9 makes it a drum part, as the oracle's kit cases are");
    render_note->add_flag("--per-note", per_note_renderer,
                          "Use the retired per-note renderer instead of the block loop. It takes "
                          "the ideal pow(2,x/12000) for its rate where every voice in the block "
                          "loop goes through g_ramp_exp_tbl, so its pitch sits up to 4.66 cents "
                          "off the module's -- do not compare its output against an oracle case");

    fs::path midi_path;
    ts::RenderOptions render_options;
    bool gsws = false;
    bool no_reverb = false;
    bool no_chorus = false;
    bool no_delay = false;
    bool no_efx = false;
    bool stream = false;
    int polyphony = ts::ToneGeneratorOptions::unlimited_polyphony;
    int ports = ts::ToneGeneratorOptions{}.ports;
    CLI::App* render = app.add_subcommand("render", "Render a Standard MIDI File to a WAV.");
    add_dll(render);
    render->add_option("midi", midi_path, "Input .mid path")->required();
    render->add_option("output", output_file, "Output .wav path")->required();
    CLI::Option* render_map =
        render->add_option("--map", map,
                           "Tone map: 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820, or xg to start in "
                           "XG mode (names accepted: sc55, sc88, sc88pro, sc8820, xg)")
            ->transform(CLI::CheckedTransformer(ts::tone_map_choices(), CLI::ignore_case));
    render->add_option(
        "--tail", render_options.tail_seconds, "Seconds to render past the last note");
    int chorus_phase = 0;
    render->add_option("--chorus-phase", chorus_phase,
                       "Place the chorus LFO accumulator before rendering. Its 24-bit counter is "
                       "never reset on the module, so a reference render's wet depends on how long "
                       "that engine ran first; scdec prints the value it reached");
    render->add_option("--end", render_options.end_seconds, "Stop at this many seconds");
    render->add_option("--volume", render_options.output_gain, "Linear output gain");
    render->add_option("--drum-map", render_options.drum_map_row, "Drum map row, 0-5");
    render->add_flag("--no-reverb", no_reverb, "Disable the reverb send");
    render->add_flag("--no-chorus", no_chorus, "Disable the chorus send");
    render->add_flag("--no-delay", no_delay, "Disable the delay send");
    render->add_flag("--no-efx", no_efx, "Disable the insertion EFX block");
    render->add_flag("--gsws", gsws,
                     "Render it the way the Microsoft GS Wavetable Synth does: the SC-55 map, "
                     "with reverb, chorus, delay and EFX all off. An explicit --map still wins");
    // Every render goes through the block loop now; what `--stream` selects is the hardware's
    // voice limit, so the stealing can be heard as the module would do it. The default is
    // unlimited, where every note in the file sounds.
    render->add_flag("--stream", stream, "Limit polyphony to the hardware's 64 voices");
    render->add_option(
        "--polyphony", polyphony, "Voice limit; 0 grows on demand instead of stealing (default)");
    render->add_option("--ports", ports,
                       "MIDI ports: 1, 2 (hardware, default) or 4. Four gives 64 parts, past what "
                       "the module has -- raise --polyphony to suit");

    std::vector<std::string> muted;
    std::vector<std::string> soloed;
    // One value per occurrence, so the list is comma-separated rather than space-separated:
    // `--mute 1,2,3`, or the option repeated. Left unbounded these take every argument that
    // follows, positionals included, and `--mute 1 2 3 in.mid out.wav` then swallows the paths --
    // which is worse than an error, because CLI11 fills the positionals from what is left and the
    // render proceeds having muted something other than what was asked for. `apply_channels`
    // already splits on commas, so only the arity was ever wrong.
    render->add_option(
        "--mute", muted,
        "Silence these channels, comma-separated and labelled as a mixer does "
        "(e.g. --mute 1,2,5; 1-64, and 17+ need --ports)")
        ->expected(1);
    render->add_option("--solo", soloed, "Hear only these channels, comma-separated")->expected(1);

    int loops = 1;
    double fade_seconds = 7.0;
    render->add_option("--loops", loops,
                       "Play the file's loop body this many times, then fade out (needs loop "
                       "points: markers, CC 111, or the XMI/Touhou controller pairs)");
    render->add_option("--fade", fade_seconds, "The post-loop fade length in seconds");

    std::string effect_kind;
    int effect_type = 0;
    int effect_samples = 32000;
    CLI::App* dump_effect =
        app.add_subcommand("dump-effect", "Render a send effect's impulse response.");
    dump_effect->add_option("kind", effect_kind, "reverb, chorus or delay")->required();
    dump_effect->add_option("type", effect_type, "GS type number")->required();
    dump_effect->add_option("samples", effect_samples, "How many samples to render")->required();
    dump_effect->add_option("output", output_file, "Output .f32 path")->required();

    CLI::App* pitch = app.add_subcommand(
        "pitch", "Print the pitch chain for one note, term by term, without rendering.");
    add_dll(pitch);
    pitch->add_option("program", program, "Program number, 0-127")->required();
    pitch->add_option("note", note, "MIDI note, 0-127")->required();
    pitch->add_option("velocity", velocity, "MIDI velocity, 1-127")->required();
    pitch->add_option("map", map, "Tone map, 1-4")->required();
    pitch->add_option("--bank", lookup_bank, "Lookup bank, as CC#0 selects it (default 0)");

    int iterations = 3;
    CLI::App* bench = app.add_subcommand("bench", "Time the render path stage by stage.");
    add_dll(bench);
    bench->add_option("midi", midi_path, "A MIDI file, to also time the two sequence stages");
    bench->add_option("--iterations", iterations, "Runs per stage; the fastest is reported");

    std::vector<fs::path> scan_paths;
    CLI::App* scan_efx = app.add_subcommand(
        "scan-efx", "Tag files with the insertion-EFX traffic they carry, one JSON row each.");
    add_dll(scan_efx);
    scan_efx->add_option("files", scan_paths, "Music files to scan")->required();

    CLI11_PARSE(app, argc, argv);

    try {
        // Resolved once, after parsing, so every subcommand reports the same thing when nothing is
        // pinned and nothing was passed.
        const ts::RomLocation rom = ts::locate_rom(dll_path);
        // Only `manifest` works without the DLL: it prints the offset map, which is this project's
        // own. Everything else -- `dump-effect` included, now that the effect coefficients are
        // computed from the DLL rather than shipped -- needs the file.
        if (!rom.found() && !manifest->parsed()) {
            throw std::runtime_error(ts::rom_not_found_message(rom));
        }
        const std::string dll = rom.path.string();

        if (manifest->parsed()) {
            return manifest_command();
        }
        if (info->parsed()) {
            return info_command(dll);
        }
        if (extract->parsed()) {
            return extract_tables_command(dll, output_directory);
        }
        if (export_soundfont->parsed()) {
            return export_soundfont_command(dll, soundfont_output, soundfont_codec, !no_maps,
                                            !no_gs_modulators);
        }
        if (render->parsed()) {
            // The GS Wavetable Synth is the SC-55 sound set with none of the effect blocks, so
            // the flag is those five settings at once. It only supplies the map, rather than
            // forcing it, so that --map keeps meaning what it says when the two are given
            // together.
            if (gsws) {
                if (render_map->count() == 0) {
                    map = static_cast<int>(ts::ToneMap::sc55);
                }
                no_reverb = true;
                no_chorus = true;
                no_delay = true;
                no_efx = true;
            }

            render_options.reverb = !no_reverb;
            render_options.chorus = !no_chorus;
            render_options.delay = !no_delay;
            render_options.efx = !no_efx;

            ts::ChannelMask mask;
            apply_channels(mask, muted, /*mute=*/true);
            apply_channels(mask, soloed, /*mute=*/false);
            if (!mask.is_default()) {
                render_options.channels = &mask;
            }

            return render_command(chorus_phase, dll, midi_path, output_file, map,
                                  render_options, stream,
                                  polyphony, ports, loops, fade_seconds);
        }
        if (bench->parsed()) {
            return bench_command(dll, midi_path, iterations);
        }
        if (scan_efx->parsed()) {
            return scan_efx_command(dll, scan_paths);
        }
        if (dump_effect->parsed()) {
            return dump_effect_command(dll, effect_kind, effect_type, effect_samples, output_file);
        }
        if (render_note->parsed()) {
            if (per_note_renderer) {
                return render_note_command(
                    dll, program, note, velocity, hold_seconds, output_file, map);
            }
            return render_note_block_loop(dll, program, note, velocity, hold_seconds,
                                          note_tail_seconds, note_channel, output_file, map);
        }
        if (pitch->parsed()) {
            return pitch_command(dll, program, note, velocity, map, lookup_bank);
        }
    } catch (const ts::RomIdentityError& error) {
        std::cerr << "tabula-sonora: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "tabula-sonora: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
