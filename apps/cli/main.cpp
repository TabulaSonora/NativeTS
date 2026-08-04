#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/render_options.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/rom_locator.hpp"
#include "tabulasonora/send_effects.hpp"
#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/smf_reader.hpp"
#include "tabulasonora/table_manifest.hpp"
#include "tabulasonora/wav_writer.hpp"
#include "tabulasonora/wave_rom.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
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

    std::ofstream stream{output, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("Cannot write '" + output.string() + "'.");
    }

    for (std::size_t i = 0; i < voice.left.size(); ++i) {
        const float frame[2]{voice.left[i], voice.right[i]};
        stream.write(reinterpret_cast<const char*>(frame), sizeof(frame));
    }
    if (!stream) {
        throw std::runtime_error("Short write to '" + output.string() + "'.");
    }

    std::cout << voice.name << ": " << voice.left.size() << " frames\n";
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
int render_command(const std::string& dll,
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

} // namespace

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

    int program = 0;
    int note = 60;
    int velocity = 100;
    double hold_seconds = 1.0;
    int map = 4;
    fs::path output_file;
    CLI::App* render_note =
        app.add_subcommand("render-note", "Render one note to raw interleaved float32.");
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

    fs::path midi_path;
    ts::RenderOptions render_options;
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
    render->add_option("--map", map,
                   "Tone map: 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820, or xg to start in XG mode "
                   "(names accepted: sc55, sc88, sc88pro, sc8820, xg)")
        ->transform(CLI::CheckedTransformer(ts::tone_map_choices(), CLI::ignore_case));
    render->add_option(
        "--tail", render_options.tail_seconds, "Seconds to render past the last note");
    render->add_option("--end", render_options.end_seconds, "Stop at this many seconds");
    render->add_option("--volume", render_options.output_gain, "Linear output gain");
    render->add_option("--drum-map", render_options.drum_map_row, "Drum map row, 0-5");
    render->add_flag("--no-reverb", no_reverb, "Disable the reverb send");
    render->add_flag("--no-chorus", no_chorus, "Disable the chorus send");
    render->add_flag("--no-delay", no_delay, "Disable the delay send");
    render->add_flag("--no-efx", no_efx, "Disable the insertion EFX block");
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
    render->add_option(
        "--mute", muted,
        "Silence these channels, as a mixer labels them (1-64; 17+ need --ports)");
    render->add_option("--solo", soloed, "Hear only these channels");

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

    int iterations = 3;
    CLI::App* bench = app.add_subcommand("bench", "Time the render path stage by stage.");
    add_dll(bench);
    bench->add_option("midi", midi_path, "A MIDI file, to also time the two sequence stages");
    bench->add_option("--iterations", iterations, "Runs per stage; the fastest is reported");

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
        if (render->parsed()) {
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

            return render_command(dll, midi_path, output_file, map, render_options, stream,
                                  polyphony, ports, loops, fade_seconds);
        }
        if (bench->parsed()) {
            return bench_command(dll, midi_path, iterations);
        }
        if (dump_effect->parsed()) {
            return dump_effect_command(dll, effect_kind, effect_type, effect_samples, output_file);
        }
        if (render_note->parsed()) {
            return render_note_command(
                dll, program, note, velocity, hold_seconds, output_file, map);
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
