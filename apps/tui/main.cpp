#include "playback.hpp"
#include "sources.hpp"

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/patch_directory.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/rom_locator.hpp"
#include "tabulasonora/render_options.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ftxui;

/// Formats a frame count as `mm:ss`.
[[nodiscard]] std::string clock_time(std::int64_t frames, int rate)
{
    const std::int64_t total = std::max<std::int64_t>(frames, 0) / rate;
    std::ostringstream text;
    text << (total / 60) << ':' << (total % 60 < 10 ? "0" : "") << (total % 60);
    return text.str();
}

/// Level in dB mapped onto a bar, floored at -48 dB.
///
/// Decibels rather than amplitude, because a linear bar spends nearly all of its length on the top
/// 6 dB and reads as either full or empty. The colour changes at -6 and 0 dB, which is where a
/// listener starts caring.
[[nodiscard]] Element level_bar(float peak, int width)
{
    const double db = peak <= 0.0F ? -120.0 : 20.0 * std::log10(static_cast<double>(peak));
    const double fraction = std::clamp((db + 48.0) / 48.0, 0.0, 1.0);
    const int filled = static_cast<int>(fraction * width);

    Color colour = Color::Green;
    if (db > -6.0) {
        colour = Color::Yellow;
    }
    if (db >= 0.0) {
        colour = Color::Red;
    }

    std::string bar;
    for (int i = 0; i < width; ++i) {
        bar += i < filled ? "█" : "·";
    }
    return text(bar) | color(colour);
}

/// A small bar showing how many voices a channel is holding.
///
/// A muted channel still shows them, and that is not a display bug: the block loop silences a part
/// where it mixes, not where it starts notes, so a muted part goes on consuming polyphony and
/// unmuting it resumes the note already in progress rather than waiting for the next one.
[[nodiscard]] Element voice_bar(int voices, int width)
{
    const int filled = std::clamp(voices, 0, width);
    std::string bar;
    for (int i = 0; i < width; ++i) {
        bar += i < filled ? "▮" : "·";
    }
    return text(bar) | color(voices > 0 ? Color::Cyan : Color::GrayDark);
}

/// Names programs, so the mixer reads as instruments rather than numbers.
///
/// Cached because the lookup walks the three-level program table and builds a string, and a redraw
/// asks for all sixteen at twenty frames a second. The directory itself is immutable once loaded,
/// so reading it from the UI thread while the render thread renders is safe.
class ProgramNames {
public:
    ProgramNames(const ts::PatchDirectory& directory, const ts::DrumKitTable& drums)
        : directory_(&directory), drums_(&drums)
    {
    }

    /// Names a melodic part's current program on the map it currently resolves against.
    ///
    /// The map is a parameter rather than a constructor argument because it is not fixed: a bank
    /// LSB names a vintage, and XG System On moves every part onto the XG map mid-song. It is part
    /// of the cache key for the same reason -- one program and bank name two different instruments
    /// on two maps, and a cache that ignored the map would show whichever was asked for first.
    [[nodiscard]] const std::string& name(int program, int bank, int map)
    {
        const int key = (map << 16) | (bank << 8) | program;
        if (const auto found = cache_.find(key); found != cache_.end()) {
            return found->second;
        }

        std::string resolved = "--";
        const int tone = directory_->program_to_tone(program, static_cast<ts::ToneMap>(map), bank);
        if (tone >= 0) {
            if (const std::optional<ts::Tone> record = directory_->tone(tone);
                record && record->is_defined()) {
                resolved = record->name();
            }
        }

        return cache_.emplace(key, std::move(resolved)).first->second;
    }

    /// Names a drum kit from its record, rather than showing the index.
    [[nodiscard]] const std::string& kit(int index)
    {
        const int key = kit_key_base | index;
        if (const auto found = cache_.find(key); found != cache_.end()) {
            return found->second;
        }

        std::string resolved = index >= 0 ? drums_->kit_name(index) : std::string{};
        if (resolved.empty()) {
            resolved = index >= 0 ? "Kit " + std::to_string(index) : "--";
        }
        return cache_.emplace(key, std::move(resolved)).first->second;
    }

private:
    /// Kits share the cache with programs; this keeps their keys clear of any (map, bank, program).
    static constexpr int kit_key_base = 1 << 28;

    const ts::PatchDirectory* directory_;
    const ts::DrumKitTable* drums_;
    std::map<int, std::string> cache_;
};

/// Everything the UI thread owns.
struct Ui {
    ts::player::Playback* playback = nullptr;
    ts::ChannelMask* channels = nullptr;
    ProgramNames* names = nullptr;

    std::string title;
    std::string subtitle;

    int selected = 0;

    /// The parts the file addresses, in order; the mixer lists exactly these.
    std::vector<int> listed;

    /// The next listed part in a direction, or the current one at either end.
    [[nodiscard]] int neighbour(int part, int direction) const
    {
        if (listed.empty()) {
            return std::clamp(part + direction, 0, 15);
        }
        const auto at = std::find(listed.begin(), listed.end(), part);
        if (at == listed.end()) {
            return listed.front();
        }
        const auto index = static_cast<std::ptrdiff_t>(at - listed.begin()) + direction;
        if (index < 0 || index >= static_cast<std::ptrdiff_t>(listed.size())) {
            return part;
        }
        return listed[static_cast<std::size_t>(index)];
    }
    float held_left = 0.0F;
    float held_right = 0.0F;
};

/// The output gain, as a multiplier rather than a dB figure -- it is a trim, not a fader.
[[nodiscard]] std::string gain_label(float gain)
{
    std::ostringstream text;
    text.precision(2);
    text << "x" << std::fixed << gain;
    return text.str();
}

[[nodiscard]] Element transport_panel(Ui& ui)
{
    ts::player::Playback& playback = *ui.playback;

    const std::int64_t length = playback.length();
    const std::int64_t at = playback.position();
    const float fraction =
        length == 0 ? 0.0F
                    : static_cast<float>(static_cast<double>(at) / static_cast<double>(length));

    // A slow decay, so a peak stays legible for a moment instead of flickering at the frame rate.
    const auto [left, right] = playback.peak();
    ui.held_left = std::max(left, ui.held_left * 0.8F);
    ui.held_right = std::max(right, ui.held_right * 0.8F);

    Element underruns = text("");
    if (const std::int64_t missed = playback.underruns(); missed > 0) {
        // Underruns are the one thing a player should never hide: a glitch the listener hears but
        // the display does not mention looks like a fault in the engine.
        underruns = text("  " + std::to_string(missed) + " underrun" + (missed == 1 ? "" : "s"))
                    | color(Color::Red);
    }

    const ts::player::EngineSnapshot state = playback.snapshot();

    return vbox({
        hbox({
            text(playback.paused() ? " ‖ " : " ▶ ") | bold
                | color(playback.paused() ? Color::Yellow : Color::Green),
            gauge(fraction) | flex,
            text("  " + clock_time(at, playback.sample_rate()) + " / "
                 + clock_time(length, playback.sample_rate())),
        }),
        hbox({
            text(" L ") | dim,
            level_bar(ui.held_left, 14),
            text(" R ") | dim,
            level_bar(ui.held_right, 14),
            // The configured limit, not the hardware's -- this player runs 256 by default and a
            // reading against 64 would look permanently overloaded. A growing pool has no ceiling,
            // so its capacity is shown as what it has reached, marked.
            text("  " + std::to_string(state.active_voices) + "/"
                 + std::to_string(state.voice_capacity) + (state.voices_grow ? "+" : ""))
                | dim,
            text("  " + std::to_string(state.note_count) + " notes") | dim,
            underruns,
            filler(),
            text(gain_label(playback.gain()) + " ") | dim,
        }),
    });
}

[[nodiscard]] Element mixer_panel(Ui& ui)
{
    const ts::player::EngineSnapshot state = ui.playback->snapshot();

    if (!state.live) {
        return vbox({
            filler(),
            text("This source is a finished render, so there is no engine to mix.") | dim | center,
            filler(),
        });
    }

    const bool soloing = ui.channels != nullptr && ui.channels->any_soloed();

    // Only the parts the file addresses. A four-port score has sixty-four to choose from and
    // typically uses a third of them; listing the rest would bury the ones that matter. A file that
    // uses one port still shows its channels in the order it always did.
    std::vector<int>& listed = ui.listed;
    listed.clear();
    for (int part = 0; part < state.part_count && part < static_cast<int>(state.parts.size());
         ++part) {
        if (state.parts[static_cast<std::size_t>(part)].present) {
            listed.push_back(part);
        }
    }
    if (listed.empty()) {
        // Before the first event lands there is nothing to list, and an empty mixer reads as a
        // fault. Fall back to the first port, which is what every single-port file will settle on.
        for (int part = 0; part < 16; ++part) {
            listed.push_back(part);
        }
    }

    const bool multi_port = !listed.empty() && listed.back() >= 16;

    Elements rows;
    rows.push_back(hbox({
                       text(multi_port ? " port ch " : " ch ") | dim,
                       text("program      ") | dim,
                       text(" bank") | dim,
                       text("  vol") | dim,
                       text("  exp") | dim,
                       text("  pan") | dim,
                       text("  voices  ") | dim,
                       text("  M S") | dim,
                   })
                   | bold);

    for (std::size_t row_index = 0; row_index < listed.size(); ++row_index) {
        const int part_index = listed[row_index];
        const int channel = part_index % 16;
        const int port = part_index / 16;
        const ts::player::PartSnapshot& part = state.parts[static_cast<std::size_t>(part_index)];
        // From the engine, not from the channel number: GS reroutes a part to the drum path over
        // SysEx and XG does it from bank select, so channel 10 is neither necessary nor sufficient.
        const bool drums = part.drums;
        const bool audible = soloing ? part.soloed : !part.muted;

        std::string label = drums ? ui.names->kit(part.kit)
                                  : ui.names->name(part.program, part.lookup_bank, part.map);
        label.resize(13, ' ');

        std::ostringstream number;
        if (multi_port) {
            // Ports are labelled A..D the way a multi-port interface labels them, which reads
            // better beside a channel number than two numbers would.
            number << ' ' << static_cast<char>('A' + port) << "   ";
        }
        number << (channel + 1 < 10 ? " " : "") << (channel + 1);

        Element row = hbox({
            text(" " + number.str() + " "),
            text(label) | (drums ? color(Color::Magenta) : color(Color::White)),
            text(std::string(5 - std::to_string(part.bank).size(), ' ') + std::to_string(part.bank))
                | dim,
            text(std::string(5 - std::to_string(part.volume).size(), ' ')
                 + std::to_string(part.volume)),
            text(std::string(5 - std::to_string(part.expression).size(), ' ')
                 + std::to_string(part.expression)),
            text(std::string(5 - std::to_string(part.pan).size(), ' ') + std::to_string(part.pan)),
            text("  "),
            voice_bar(part.voices, 8),
            text("  "),
            text(part.muted ? "M" : "·") | color(part.muted ? Color::Red : Color::GrayDark),
            text(" "),
            text(part.soloed ? "S" : "·") | color(part.soloed ? Color::Yellow : Color::GrayDark),
        });

        if (!audible) {
            row = row | dim;
        }
        if (part_index == ui.selected) {
            // `focus` is what scrolls the frame below: on a terminal too short for the list the
            // selected row is the one guaranteed to stay visible.
            row = row | inverted | focus;
        }
        rows.push_back(row);
    }

    return vbox(std::move(rows));
}

/// The key hints, as one string rather than a row of elements.
///
/// An `hbox` of many texts shrinks each one when the terminal is narrow, which eats the spaces
/// between words and turns this into gibberish; a single text truncates from the right instead.
/// It is sized to fit eighty columns, which is why `+`/`-` for gain is left to `--help` -- the
/// trim is already visible in the transport line.
[[nodiscard]] Element help_bar()
{
    return text(" space pause  \u2190\u2192 5s  ,. 30s  home  \u2191\u2193 ch  m mute  "
                "s solo  r reset  q quit")
           | dim;
}

int run(const std::string& dll,
        const fs::path& midi,
        const ts::RenderOptions& options,
        const ts::player::DeviceOptions& device,
        ts::ChannelMask& channels)
{
    const ts::RomImage rom = ts::RomImage::open(dll, ts::RomVerification::quick);
    ts::NoteRenderer notes{rom};

    ts::ToneGeneratorOptions engine;

    // Four ports by default here, unlike the CLI. A player is asked to play whatever it is given,
    // and a four-port score folded onto two collides its parts silently -- the one failure mode a
    // listener cannot diagnose. The polyphony goes up to match, since sixty-four parts sharing the
    // hardware's sixty-four voices would steal without pause.
    engine.ports = ts::ToneGenerator::max_port_count;
    engine.polyphony = 256;

    engine.map = options.map;
    engine.drum_channel = options.drum_channel;
    engine.reverb = options.reverb;
    engine.chorus = options.chorus;
    engine.delay = options.delay;

    // Always attached, unlike the offline path's optional mask: the mixer has to be able to mute a
    // channel that started out audible, and the engine reads this pointer live.
    engine.channels = &channels;

    auto streaming =
        std::make_unique<ts::player::StreamingSource>(notes, engine, midi, options.tail_seconds);

    ts::player::Playback playback{std::move(streaming), device};
    ProgramNames names{notes.directory(), notes.drums()};

    Ui ui;
    ui.playback = &playback;
    ui.channels = &channels;
    ui.names = &names;
    ui.title = midi.filename().string();

    static const char* const map_names[] = {"", "SC-55", "SC-88", "SC-88Pro", "SC-8820"};
    const auto map_index = static_cast<std::size_t>(options.map);
    ui.subtitle = std::string{map_index < 5 ? map_names[map_index] : "?"} + " · "
                  + playback.backend_name() + " · " + playback.device_name() + " · "
                  + std::to_string(playback.device_rate()) + " Hz · "
                  + std::to_string(device.period_frames) + " frames";

    playback.start();

    ScreenInteractive screen = ScreenInteractive::Fullscreen();

    Component view = Renderer([&] {
        return vbox({
                   hbox({
                       text(" " + ui.title) | bold | color(Color::Cyan),
                       text(" \u00b7 " + ui.subtitle) | dim,
                   }),
                   separator(),
                   transport_panel(ui),
                   separator(),
                   // The mixer is the part that gives: everything else is one or two lines and has
                   // to stay on screen, so a terminal too short for sixteen channels scrolls this
                   // rather than pushing the help bar off the bottom.
                   mixer_panel(ui) | vscroll_indicator | yframe | flex,
                   separator(),
                   help_bar(),
               })
               | border;
    });

    view = view | CatchEvent([&](const Event& event) {
               if (event == Event::Character('q') || event == Event::Escape) {
                   screen.Exit();
                   return true;
               }
               if (event == Event::Character(' ')) {
                   playback.set_paused(!playback.paused());
                   return true;
               }
               if (event == Event::ArrowLeft) {
                   playback.seek_by(-5.0);
                   return true;
               }
               if (event == Event::ArrowRight) {
                   playback.seek_by(5.0);
                   return true;
               }
               if (event == Event::Character(',')) {
                   playback.seek_by(-30.0);
                   return true;
               }
               if (event == Event::Character('.')) {
                   playback.seek_by(30.0);
                   return true;
               }
               if (event == Event::Home) {
                   playback.seek(0);
                   return true;
               }
               if (event == Event::ArrowUp) {
                   // Step through the parts the file uses, not the index space: on a four-port
                   // file most indices are empty and arrowing through them would feel broken.
                   ui.selected = ui.neighbour(ui.selected, -1);
                   return true;
               }
               if (event == Event::ArrowDown) {
                   ui.selected = ui.neighbour(ui.selected, +1);
                   return true;
               }
               if (event == Event::Character('m')) {
                   channels.set_muted(ui.selected, !channels.is_muted(ui.selected));
                   return true;
               }
               if (event == Event::Character('s')) {
                   channels.set_soloed(ui.selected, !channels.is_soloed(ui.selected));
                   return true;
               }
               if (event == Event::Character('r')) {
                   channels.reset();
                   return true;
               }
               if (event == Event::Character('+') || event == Event::Character('=')) {
                   playback.set_gain(std::min(4.0F, playback.gain() * 1.15F));
                   return true;
               }
               if (event == Event::Character('-')) {
                   playback.set_gain(std::max(0.02F, playback.gain() / 1.15F));
                   return true;
               }
               return false;
           });

    // `Loop` blocks until something arrives, so the meters would freeze between key presses without
    // something to wake it. Twenty a second is smooth and costs nothing next to the synthesis.
    std::atomic<bool> running{true};
    std::thread ticker{[&] {
        while (running.load(std::memory_order_relaxed)) {
            screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
    }};

    screen.Loop(view);

    running.store(false, std::memory_order_relaxed);
    ticker.join();
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
    CLI::App app{"A full-screen mixer over the Sound Canvas engine.", "tabula-sonora-tui"};
    app.footer("Keys: space pause, arrows seek 5s, , / . seek 30s, home restart, up/down\n"
               "select a channel, m mute, s solo, r reset the mixer, + / - trim the gain,\n"
               "q quit.\n\n"
               "The ROM is found from --dll, then $TS_SCCORE_DLL, then ./SCCore.dll.");

    bool devices = false;
    app.add_flag("--list-devices", devices, "List the output devices and exit");

    std::string dll_path;
    fs::path midi_path;
    app.add_option("midi", midi_path, "Input .mid path");
    app.add_option("--dll", dll_path, "Path to SCCore.dll; overrides TS_SCCORE_DLL");

    int map = 4;
    ts::RenderOptions options;
    ts::player::DeviceOptions device;
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
    app.add_flag("--no-reverb", no_reverb, "Disable the reverb send");
    app.add_flag("--no-chorus", no_chorus, "Disable the chorus send");
    app.add_flag("--no-delay", no_delay, "Disable the delay send");
    app.add_option("--mute", muted,
                   "Start with these channels silenced (1-64; 17+ are ports B-D)");
    app.add_option("--solo", soloed, "Start with only these channels audible");

    CLI11_PARSE(app, argc, argv);

    try {
        if (devices) {
            const std::vector<ts::player::DeviceInfo> found = ts::player::output_devices();
            std::cout << found.size() << " output device(s):\n";
            for (std::size_t i = 0; i < found.size(); ++i) {
                std::cout << "  [" << i << "] " << found[i].name
                          << (found[i].is_default ? "  (default)" : "") << '\n';
            }
            return 0;
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

        ts::ChannelMask channels;
        apply_channels(channels, muted, /*mute=*/true);
        apply_channels(channels, soloed, /*mute=*/false);

        return run(rom.path.string(), midi_path, options, device, channels);
    } catch (const ts::RomIdentityError& error) {
        std::cerr << "tabula-sonora-tui: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "tabula-sonora-tui: " << error.what() << '\n';
        return 1;
    }
}
