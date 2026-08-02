#include "sources.hpp"
#include "terminal.hpp"

#include "tabulasonora/frame_ring.hpp"
#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/sequence_renderer.hpp"

#include <miniaudio.h>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using ts::FrameRing;
using ts::player::Key;

/// Frames the device asks for at a time, unless `--buffer` says otherwise.
constexpr std::uint32_t default_period_frames = 512;

/// How far ahead of the device the render thread keeps the ring, in milliseconds.
constexpr int default_latency_ms = 150;

/// The audio callback: a copy out of the ring and nothing else.
///
/// This runs on a real-time thread. It allocates nothing, takes no lock and calls into no engine
/// code -- everything it plays was rendered ahead of time by the render thread. A block the
/// renderer did not manage to produce comes out as silence and is counted, not waited for.
void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frames)
{
    (void)input;
    auto* ring = static_cast<FrameRing*>(device->pUserData);
    ring->read(std::span{static_cast<float*>(output), static_cast<std::size_t>(frames) * 2});
}

/// Formats a frame count as `mm:ss`.
[[nodiscard]] std::string clock_time(std::int64_t frames, int rate)
{
    const auto total = static_cast<std::int64_t>(std::max<std::int64_t>(frames, 0) / rate);
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

/// Lists the playback devices the backend can see.
int list_devices()
{
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        std::cerr << "tabula-sonora-play: cannot initialise the audio backend.\n";
        return 1;
    }

    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&context, &playback, &count, &capture, &capture_count)
        != MA_SUCCESS) {
        ma_context_uninit(&context);
        std::cerr << "tabula-sonora-play: cannot enumerate output devices.\n";
        return 1;
    }

    std::cout << count << " output device(s):\n";
    for (ma_uint32 i = 0; i < count; ++i) {
        std::cout << "  [" << i << "] " << playback[i].name
                  << (playback[i].isDefault != 0 ? "  (default)" : "") << '\n';
    }

    ma_context_uninit(&context);
    return 0;
}

/// Resolves `--device` to an entry in the backend's list.
///
/// Accepts an index or a case-insensitive substring of the name, which is what a person actually
/// has to hand after reading `--list-devices`.
[[nodiscard]] bool find_device(ma_context& context, const std::string& wanted, ma_device_id& id)
{
    ma_device_info* playback = nullptr;
    ma_uint32 count = 0;
    ma_device_info* capture = nullptr;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&context, &playback, &count, &capture, &capture_count)
        != MA_SUCCESS) {
        std::cerr << "tabula-sonora-play: cannot enumerate output devices.\n";
        return false;
    }

    if (wanted.find_first_not_of("0123456789") == std::string::npos && !wanted.empty()) {
        const auto index = static_cast<ma_uint32>(std::stoul(wanted));
        if (index >= count) {
            std::cerr << "tabula-sonora-play: no output device with index " << index
                      << "; there are " << count << ".\n";
            return false;
        }
        id = playback[index].id;
        std::cout << "  output: " << playback[index].name << '\n';
        return true;
    }

    std::string needle = wanted;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    for (ma_uint32 i = 0; i < count; ++i) {
        std::string name = playback[i].name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (name.find(needle) != std::string::npos) {
            id = playback[i].id;
            std::cout << "  output: " << playback[i].name << '\n';
            return true;
        }
    }

    std::cerr << "tabula-sonora-play: no output device matching '" << wanted
              << "'. Try --list-devices.\n";
    return false;
}

/// What the transport thread and the UI thread pass between them.
///
/// A mutex is fine here -- neither of these threads is the audio callback, and the callback never
/// touches this. Seeks are queued rather than applied in place because the render thread owns the
/// source and is the only thread allowed to move it.
struct Transport {
    std::atomic<bool> quit{false};
    std::atomic<bool> finished{false};
    std::atomic<std::int64_t> audible{0};

    std::mutex mutex;
    std::optional<std::int64_t> pending_seek;
};

/// Renders ahead of the device, filling the ring.
///
/// Everything expensive happens here: the block loop, the effects, the sampler. A slow block eats
/// into the ring's lead rather than glitching the device outright, which is the whole reason the
/// two are separated.
void render_loop(ts::player::PlaybackSource& source,
                 FrameRing& ring,
                 Transport& transport,
                 float gain,
                 std::size_t block_frames)
{
    std::vector<float> block(block_frames * 2, 0.0F);

    while (!transport.quit.load(std::memory_order_relaxed)) {
        std::optional<std::int64_t> seek;
        {
            const std::lock_guard<std::mutex> guard{transport.mutex};
            seek = std::exchange(transport.pending_seek, std::nullopt);
        }

        if (seek) {
            // Drop what is queued so the new position is heard now rather than a ring's worth of
            // audio later. The consumer performs the drop; nothing may be written until it has.
            ring.set_starvation_expected(true);
            ring.request_flush();
            source.set_position(*seek);
        }

        if (ring.flush_pending()) {
            std::this_thread::sleep_for(std::chrono::microseconds{500});
            continue;
        }

        if (ring.queued() >= ring.capacity() / 2) {
            ring.set_starvation_expected(false);
        }

        if (source.at_end() && ring.queued() == 0) {
            transport.finished.store(true, std::memory_order_release);
            return;
        }

        if (ring.writable() < block_frames) {
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
            continue;
        }

        source.read(block, gain);
        ring.write(block);

        transport.audible.store(source.position() - static_cast<std::int64_t>(ring.queued()),
                                std::memory_order_relaxed);
    }
}

/// Draws the one-line transport display.
void draw(const ts::player::PlaybackSource& source,
          const FrameRing& ring,
          const Transport& transport,
          bool paused,
          float& held_left,
          float& held_right)
{
    constexpr int width = 32;

    const int rate = source.sample_rate();
    const std::int64_t length = source.length();
    const std::int64_t at =
        std::clamp(transport.audible.load(std::memory_order_relaxed), std::int64_t{0}, length);

    const double fraction =
        length == 0 ? 0.0 : static_cast<double>(at) / static_cast<double>(length);
    const int filled = std::clamp(static_cast<int>(fraction * width), 0, width);

    std::string bar(static_cast<std::size_t>(filled), '=');
    if (filled < width) {
        bar += '>';
    }
    bar.resize(width, ' ');

    // A slow decay, so a peak stays legible for a moment instead of flickering at the frame rate.
    const auto [left, right] = ring.peak();
    held_left = std::max(left, held_left * 0.75F);
    held_right = std::max(right, held_right * 0.75F);

    const std::int64_t underruns = ring.underruns();
    std::ostringstream line;
    line << "\r  " << (paused ? "||" : " >") << " [" << bar << "] " << clock_time(at, rate) << " / "
         << clock_time(length, rate) << "  " << meter(held_left) << meter(held_right);
    if (underruns > 0) {
        line << "  " << underruns << " underrun" << (underruns == 1 ? "" : "s");
    }
    line << "   ";

    std::cout << line.str() << std::flush;
}

int play(const std::string& dll,
         const fs::path& midi,
         const ts::RenderOptions& options,
         bool prerender,
         std::uint32_t period_frames,
         int latency_ms,
         float gain,
         const std::string& device_name)
{
    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    ts::NoteRenderer notes{rom};

    std::unique_ptr<ts::player::PlaybackSource> source;

    if (prerender) {
        std::cout << "Rendering " << midi.filename().string() << " ..." << std::flush;
        const auto started = std::chrono::steady_clock::now();

        ts::SequenceRenderer renderer{notes};
        ts::RenderResult result = renderer.render_file(midi, options);

        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - started;
        const double seconds = static_cast<double>(result.left.size()) / result.sample_rate;

        std::cout << "\r" << midi.filename().string() << "  "
                  << clock_time(static_cast<std::int64_t>(result.left.size()), result.sample_rate)
                  << "  " << result.note_count << " notes, peak " << std::fixed
                  << std::setprecision(3) << result.peak << " - rendered in "
                  << std::setprecision(1) << elapsed.count() << " s (" << std::setprecision(0)
                  << (seconds / std::max(elapsed.count(), 0.001)) << "x realtime)\n";

        if (result.peak * gain > 1.0F) {
            std::cout << "  note: peak x gain is " << std::fixed << std::setprecision(2)
                      << (result.peak * gain) << ", so the output will clip.\n";
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
        engine.drum_ring_seconds = options.drum_ring_seconds;
        engine.channels = options.channels;

        auto streaming = std::make_unique<ts::player::StreamingSource>(
            notes, engine, midi, options.tail_seconds);
        std::cout << midi.filename().string() << "  "
                  << clock_time(streaming->length(), streaming->sample_rate()) << "  streaming\n";
        source = std::move(streaming);
    }

    // Sized from the requested lead, and never smaller than a few device periods: the render thread
    // is scheduled like any other, and the ring has to cover its worst nap rather than its average.
    const auto lead_frames = static_cast<std::size_t>(std::max<std::int64_t>(
        static_cast<std::int64_t>(period_frames) * 4,
        static_cast<std::int64_t>(latency_ms) * source->sample_rate() / 1000));
    FrameRing ring{lead_frames * 2};

    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        std::cerr << "tabula-sonora-play: cannot initialise the audio backend.\n";
        return 1;
    }

    ma_device_id device_id{};
    const bool named = !device_name.empty();
    if (named && !find_device(context, device_name, device_id)) {
        ma_context_uninit(&context);
        return 1;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.playback.pDeviceID = named ? &device_id : nullptr;

    // The engine's rate is a constant, not a setting. Asking the device for 32 kHz lets miniaudio
    // convert to whatever the hardware actually runs at, which is one resampler rather than two.
    config.sampleRate = static_cast<ma_uint32>(source->sample_rate());
    config.periodSizeInFrames = period_frames;
    config.dataCallback = data_callback;
    config.pUserData = &ring;

    ma_device device;
    if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
        ma_context_uninit(&context);
        std::cerr << "tabula-sonora-play: cannot open the output device.\n";
        return 1;
    }

    Transport transport;
    std::thread renderer{[&] { render_loop(*source, ring, transport, gain, period_frames); }};

    // Fill the ring before the device starts, or the first blocks come out as counted underruns
    // through no fault of the renderer.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (ring.queued() < ring.capacity() / 2
           && !transport.finished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    bool paused = false;
    int exit_code = 0;

    if (ma_device_start(&device) != MA_SUCCESS) {
        std::cerr << "tabula-sonora-play: cannot start the output device.\n";
        exit_code = 1;
        transport.quit.store(true, std::memory_order_relaxed);
    } else {
        std::cout << "  " << ma_get_backend_name(device.pContext->backend) << " · "
                  << device.sampleRate << " Hz · " << period_frames << " frames · ~" << latency_ms
                  << " ms buffered\n"
                  << "  space pause | left/right 5s | , / . 30s | home restart | q quit\n\n";
    }

    {
        const ts::player::RawTerminal terminal;
        float held_left = 0.0F;
        float held_right = 0.0F;

        while (!transport.quit.load(std::memory_order_relaxed)) {
            const std::int64_t rate = source->sample_rate();
            bool seeked = false;
            std::int64_t target = transport.audible.load(std::memory_order_relaxed);

            for (Key key = terminal.poll(); key != Key::none; key = terminal.poll()) {
                switch (key) {
                case Key::space:
                    paused = !paused;
                    if (paused) {
                        ma_device_stop(&device);
                    } else {
                        ma_device_start(&device);
                    }
                    break;
                case Key::left:
                    target -= 5 * rate;
                    seeked = true;
                    break;
                case Key::right:
                    target += 5 * rate;
                    seeked = true;
                    break;
                case Key::comma:
                    target -= 30 * rate;
                    seeked = true;
                    break;
                case Key::period:
                    target += 30 * rate;
                    seeked = true;
                    break;
                case Key::home:
                    target = 0;
                    seeked = true;
                    break;
                case Key::quit:
                    transport.quit.store(true, std::memory_order_relaxed);
                    break;
                case Key::none:
                    break;
                }
            }

            if (seeked && !transport.quit.load(std::memory_order_relaxed)) {
                const std::lock_guard<std::mutex> guard{transport.mutex};
                transport.pending_seek = std::max<std::int64_t>(target, 0);
            }

            if (transport.finished.load(std::memory_order_acquire)) {
                break;
            }

            draw(*source, ring, transport, paused, held_left, held_right);
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        draw(*source, ring, transport, paused, held_left, held_right);
    }

    transport.quit.store(true, std::memory_order_relaxed);
    renderer.join();

    ma_device_uninit(&device);
    ma_context_uninit(&context);

    std::cout << '\n';
    return exit_code;
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
                throw std::runtime_error("Channel '" + item + "' is outside 1-16.");
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

    bool devices = false;
    app.add_flag("--list-devices", devices, "List the output devices and exit");

    std::string dll_path;
    fs::path midi_path;
    app.add_option("dll", dll_path, "Path to SCCore.dll");
    app.add_option("midi", midi_path, "Input .mid path");

    int map = 4;
    ts::RenderOptions options;
    std::string device_name;
    auto period_frames = static_cast<int>(default_period_frames);
    int latency_ms = default_latency_ms;
    auto gain = 1.0F;
    bool prerender = false;
    bool no_reverb = false;
    bool no_chorus = false;
    bool no_delay = false;
    std::vector<std::string> muted;
    std::vector<std::string> soloed;

    app.add_option("--map", map, "Tone map: 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820");
    app.add_option("--device", device_name, "Output device; an index or a substring of its name");
    app.add_option("--buffer", period_frames, "Device period, in frames");
    app.add_option("--latency", latency_ms, "How far ahead to keep the device fed, in ms");
    app.add_option("--gain", gain, "Linear output gain");
    app.add_option("--tail", options.tail_seconds, "Release tail past the last event");
    app.add_option("--drum-map", options.drum_map_row, "Drum map row, 0-5");
    app.add_flag("--prerender", prerender, "Render the whole song first instead of streaming it");
    app.add_flag("--no-reverb", no_reverb, "Disable the reverb send");
    app.add_flag("--no-chorus", no_chorus, "Disable the chorus send");
    app.add_flag("--no-delay", no_delay, "Disable the delay send");
    app.add_option("--mute", muted, "Silence these channels, as a mixer labels them (1-16)");
    app.add_option("--solo", soloed, "Hear only these channels");

    CLI11_PARSE(app, argc, argv);

    if (devices) {
        return list_devices();
    }

    if (dll_path.empty() || midi_path.empty()) {
        std::cerr << app.help() << '\n';
        return 1;
    }

    try {
        options.map = static_cast<ts::ToneMap>(map);
        options.reverb = !no_reverb;
        options.chorus = !no_chorus;
        options.delay = !no_delay;

        ts::ChannelMask mask;
        apply_channels(mask, muted, /*mute=*/true);
        apply_channels(mask, soloed, /*mute=*/false);
        if (!mask.is_default()) {
            options.channels = &mask;
        }

        return play(dll_path,
                    midi_path,
                    options,
                    prerender,
                    static_cast<std::uint32_t>(std::max(period_frames, 32)),
                    latency_ms,
                    gain,
                    device_name);
    } catch (const ts::RomIdentityError& error) {
        std::cerr << "tabula-sonora-play: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "tabula-sonora-play: " << error.what() << '\n';
        return 1;
    }
}
