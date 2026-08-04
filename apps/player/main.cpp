#include "terminal.hpp"

#include "playback.hpp"
#include "sources.hpp"

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/rom_locator.hpp"
#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/tone_generator.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using ts::player::Key;

/// Formats a frame count as `mm:ss`.
[[nodiscard]] std::string clock_time(std::int64_t frames, int rate)
{
    const std::int64_t total = std::max<std::int64_t>(frames, 0) / rate;
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << (total / 60) << ':' << std::setw(2)
         << (total % 60);
    return text.str();
}

/// One meter character: eight blocks at roughly 6 dB each, so the tallest means close to full
/// scale.
[[nodiscard]] std::string meter(float peak)
{
    static const char* const blocks[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    if (peak <= 0.0005F) {
        return blocks[0];
    }

    const double db = 20.0 * std::log10(static_cast<double>(peak));
    const auto index = static_cast<int>(std::clamp((db + 42.0) / 6.0, 0.0, 7.0));
    return blocks[index];
}

/// Draws the one-line transport display.
void draw(const ts::player::Playback& playback, float& held_left, float& held_right)
{
    constexpr int width = 32;

    const std::int64_t length = playback.length();
    const std::int64_t at = playback.position();
    const double fraction =
        length == 0 ? 0.0 : static_cast<double>(at) / static_cast<double>(length);
    const int filled = std::clamp(static_cast<int>(fraction * width), 0, width);

    std::string bar(static_cast<std::size_t>(filled), '=');
    if (filled < width) {
        bar += '>';
    }
    bar.resize(width, ' ');

    // A slow decay, so a peak stays legible for a moment instead of flickering at the frame rate.
    const auto [left, right] = playback.peak();
    held_left = std::max(left, held_left * 0.75F);
    held_right = std::max(right, held_right * 0.75F);

    std::ostringstream line;
    line << "\r  " << (playback.paused() ? "||" : " >") << " [" << bar << "] "
         << clock_time(at, playback.sample_rate()) << " / "
         << clock_time(length, playback.sample_rate()) << "  " << meter(held_left)
         << meter(held_right);

    // Underruns are the one thing a player should never hide: a glitch the listener hears but the
    // display does not mention looks like a fault in the engine.
    if (const std::int64_t missed = playback.underruns(); missed > 0) {
        line << "  " << missed << " underrun" << (missed == 1 ? "" : "s");
    }
    line << "   ";

    std::cout << line.str() << std::flush;
}

int list_devices()
{
    const std::vector<ts::player::DeviceInfo> devices = ts::player::output_devices();
    std::cout << devices.size() << " output device(s):\n";
    for (std::size_t i = 0; i < devices.size(); ++i) {
        std::cout << "  [" << i << "] " << devices[i].name
                  << (devices[i].is_default ? "  (default)" : "") << '\n';
    }
    return 0;
}

int play(const std::string& dll,
         const fs::path& midi,
         const ts::RenderOptions& options,
         bool prerender,
         const ts::player::DeviceOptions& device)
{
    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    ts::NoteRenderer notes{rom};

    std::unique_ptr<ts::player::PlaybackSource> source;

    if (prerender) {
        std::cout << "Rendering " << midi.filename().string() << " ..." << std::flush;
        const auto started = std::chrono::steady_clock::now();

        // The block loop, as everywhere else. Prerendering is now only about *when* the work
        // happens, not about which engine does it -- unlimited polyphony so nothing is stolen
        // ahead of a listen that has all the time in the world.
        ts::ToneGeneratorOptions engine_options;
        engine_options.map = options.map;
        engine_options.drum_channel = options.drum_channel;
        engine_options.reverb = options.reverb;
        engine_options.chorus = options.chorus;
        engine_options.delay = options.delay;
        engine_options.reverb_type = options.reverb_type;
        engine_options.chorus_type = options.chorus_type;
        engine_options.delay_type = options.delay_type;
        engine_options.output_gain = options.output_gain;
        engine_options.polyphony = ts::ToneGeneratorOptions::unlimited_polyphony;

        ts::ToneGenerator generator{notes, engine_options};
        generator.set_drum_map_row(options.drum_map_row);
        ts::SequencePlayer player = ts::SequencePlayer::from_file(generator, midi);
        ts::RenderResult result = player.render_to_end(options.tail_seconds, options.end_seconds);

        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
        const double seconds = static_cast<double>(result.left.size()) / result.sample_rate;

        std::cout << "\r" << midi.filename().string() << "  "
                  << clock_time(static_cast<std::int64_t>(result.left.size()), result.sample_rate)
                  << "  " << result.note_count << " notes, peak " << std::fixed
                  << std::setprecision(3) << result.peak << " - rendered in "
                  << std::setprecision(1) << elapsed.count() << " s (" << std::setprecision(0)
                  << (seconds / std::max(elapsed.count(), 0.001)) << "x realtime)\n";

        if (result.peak * device.gain > 1.0F) {
            std::cout << "  note: peak x gain is " << std::fixed << std::setprecision(2)
                      << (result.peak * device.gain) << ", so the output will clip.\n";
        }

        source = std::make_unique<ts::player::PlaybackBuffer>(
            std::move(result.left), std::move(result.right), result.sample_rate);
    } else {
        ts::ToneGeneratorOptions engine;
        engine.map = options.map;
        engine.drum_channel = options.drum_channel;
        engine.reverb = options.reverb;
        engine.chorus = options.chorus;
        engine.delay = options.delay;
        engine.channels = options.channels;

        auto streaming = std::make_unique<ts::player::StreamingSource>(
            notes, engine, midi, options.tail_seconds);
        std::cout << midi.filename().string() << "  "
                  << clock_time(streaming->length(), streaming->sample_rate()) << "  streaming\n";
        source = std::move(streaming);
    }

    ts::player::Playback playback{std::move(source), device};
    playback.start();

    std::cout << "  " << playback.backend_name() << " · " << playback.device_name() << " · "
              << playback.device_rate() << " Hz · " << device.period_frames << " frames · ~"
              << device.latency_ms << " ms buffered\n"
              << "  space pause | left/right 5s | , / . 30s | home restart | q quit\n\n";

    const ts::player::RawTerminal terminal;
    float held_left = 0.0F;
    float held_right = 0.0F;
    bool quit = false;

    while (!quit) {
        for (Key key = terminal.poll(); key != Key::none; key = terminal.poll()) {
            switch (key) {
            case Key::space:
                playback.set_paused(!playback.paused());
                break;
            case Key::left:
                playback.seek_by(-5.0);
                break;
            case Key::right:
                playback.seek_by(5.0);
                break;
            case Key::comma:
                playback.seek_by(-30.0);
                break;
            case Key::period:
                playback.seek_by(30.0);
                break;
            case Key::home:
                playback.seek(0);
                break;
            case Key::quit:
                quit = true;
                break;
            case Key::none:
                break;
            }
        }

        if (playback.finished()) {
            break;
        }

        draw(playback, held_left, held_right);
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    draw(playback, held_left, held_right);
    std::cout << '\n';
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
    CLI::App app{"Play a MIDI file through the Sound Canvas engine.", "tabula-sonora-play"};
    app.footer("The ROM is found from --dll, then $TS_SCCORE_DLL, then ./SCCore.dll.");

    bool devices = false;
    app.add_flag("--list-devices", devices, "List the output devices and exit");

    std::string dll_path;
    fs::path midi_path;
    app.add_option("midi", midi_path, "Input .mid path");
    app.add_option("--dll", dll_path, "Path to SCCore.dll; overrides TS_SCCORE_DLL");

    int map = 4;
    ts::RenderOptions options;
    ts::player::DeviceOptions device;
    bool prerender = false;
    bool no_reverb = false;
    bool no_chorus = false;
    bool no_delay = false;
    std::vector<std::string> muted;
    std::vector<std::string> soloed;

    app.add_option("--map", map,
                   "Tone map: 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820, or xg to start in XG mode "
                   "(names accepted: sc55, sc88, sc88pro, sc8820, xg)")
        ->transform(CLI::CheckedTransformer(ts::tone_map_choices(), CLI::ignore_case));
    app.add_option("--device", device.name, "Output device; an index or a substring of its name");
    app.add_option("--buffer", device.period_frames, "Device period, in frames");
    app.add_option("--latency", device.latency_ms, "How far ahead to keep the device fed, in ms");
    app.add_option("--gain", device.gain, "Linear output gain");
    app.add_option("--tail", options.tail_seconds, "Release tail past the last event");
    app.add_option("--drum-map", options.drum_map_row, "Drum map row, 0-5");
    app.add_flag("--prerender", prerender, "Render the whole song first instead of streaming it");
    app.add_flag("--no-reverb", no_reverb, "Disable the reverb send");
    app.add_flag("--no-chorus", no_chorus, "Disable the chorus send");
    app.add_flag("--no-delay", no_delay, "Disable the delay send");
    app.add_option("--mute", muted,
                   "Silence these channels, as a mixer labels them (1-32; 17+ are port B)");
    app.add_option("--solo", soloed, "Hear only these channels");

    CLI11_PARSE(app, argc, argv);

    try {
        if (devices) {
            return list_devices();
        }

        if (midi_path.empty()) {
            std::cerr << app.help() << '\n';
            return 1;
        }

        const ts::RomLocation rom = ts::locate_rom(dll_path);
        if (!rom.found()) {
            throw std::runtime_error(ts::rom_not_found_message(rom));
        }

        options.map = static_cast<ts::ToneMap>(map);
        options.reverb = !no_reverb;
        options.chorus = !no_chorus;
        options.delay = !no_delay;
        device.period_frames = std::max(device.period_frames, 32);

        ts::ChannelMask mask;
        apply_channels(mask, muted, /*mute=*/true);
        apply_channels(mask, soloed, /*mute=*/false);
        if (!mask.is_default()) {
            options.channels = &mask;
        }

        return play(rom.path.string(), midi_path, options, prerender, device);
    } catch (const ts::RomIdentityError& error) {
        std::cerr << "tabula-sonora-play: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "tabula-sonora-play: " << error.what() << '\n';
        return 1;
    }
}
