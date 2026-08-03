#include "web_session.hpp"

#include "tabulasonora/wav_writer.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ts::web {
namespace {

using nlohmann::json;

bool equals_ignore_case(const std::string& a, const std::string& b)
{
    return a.size() == b.size()
           && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                  return std::tolower(static_cast<unsigned char>(x))
                         == std::tolower(static_cast<unsigned char>(y));
              });
}

/// Stands in for a name where there is nothing to name.
constexpr const char* nothing = "—";

std::string trimmed(std::string name)
{
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    while (!name.empty() && name.front() == ' ') {
        name.erase(name.begin());
    }
    return name;
}

// The indirect-only marker is the top bit of an otherwise ordinary tone reference, and the
// program-change handler strips it the same way. Naming one is worth the mask: those entries are
// real sounds the module reaches in mono mode, and listing them unnamed would say the ROM is
// emptier than it is.
std::string indirect_name(const PatchDirectory& directory, int raw)
{
    const int reference = raw & 0x7FFF;

    if (reference < PatchDirectory::melodic_space_end) {
        if (auto tone = directory.tone(reference); tone && tone->is_defined()) {
            return trimmed(tone->name());
        }
        return nothing;
    }

    if (reference >= PatchDirectory::alternate_space_start) {
        if (auto entry = directory.alternate(reference - PatchDirectory::alternate_space_start)) {
            return entry->name;
        }
    }

    return nothing;
}

struct CatalogEntry {
    int tone = -1;
    std::string name = nothing;
    const char* kind = "unassigned";
};

json entry_json(int program, const CatalogEntry& entry)
{
    return json{{"program", program},
                {"tone", entry.tone},
                {"name", entry.name},
                {"kind", entry.kind}};
}

} // namespace

void WebSession::load_rom(std::uint8_t* data, std::size_t length, std::string name,
                          const std::string& expected_sha256)
{
    // Ownership first, so every error path below frees the transfer.
    std::unique_ptr<std::uint8_t[], FreeDeleter> bytes{data};

    const bool trusted = expected_sha256.size() == 64;
    auto rom = RomImage::from_memory(std::span<const std::uint8_t>{bytes.get(), length},
                                     trusted ? RomVerification::quick : RomVerification::full,
                                     nullptr, name);

    // A stored hash still has to match the build this library is pinned to; the quick check above
    // only skips recomputing it, it does not skip believing it.
    if (trusted && !equals_ignore_case(expected_sha256, rom.manifest().dll().sha256)) {
        throw RomIdentityError("The stored copy of '" + name + "' has SHA-256 " + expected_sha256
                               + "; the pinned build is " + rom.manifest().dll().sha256
                               + ". Pick the file again.");
    }

    unload_rom();

    rom_bytes_ = std::move(bytes);
    rom_length_ = length;
    rom_.emplace(std::move(rom));
    notes_.emplace(*rom_);
    rom_name_ = std::move(name);
    rom_verified_ = !trusted;
    rebuild();
}

void WebSession::unload_rom()
{
    export_abort();
    export_left_ = {};
    export_right_ = {};
    export_wav_ = {};
    player_.reset();
    engine_.reset();
    notes_.reset();
    rom_.reset();
    rom_bytes_.reset();
    rom_length_ = 0;
    rom_name_.clear();
    rom_verified_ = false;
}

void WebSession::load_song(std::span<const std::uint8_t> midi, std::string name)
{
    auto events = smf::parse(midi, sample_rate);

    // Which parts the file touches at all, so the mixer can show those and no others. A sixteen-part
    // file has no business drawing sixty-four strips, and the ones it would draw are parts nothing
    // can reach.
    //
    // The port is folded exactly as the engine folds it, because the question the mixer is asking is
    // "which strips will make a sound", not "what does the file say". A file tagged for four ports
    // playing on a two-port engine addresses parts on both, and its port C traffic lands on port A.
    const int ports = std::max(1, options().ports);
    std::vector<bool> seen(static_cast<std::size_t>(ChannelMask::channel_count), false);
    for (const auto& message : events) {
        if (message.kind != MidiEventKind::channel) {
            continue;
        }
        const int part = ((message.port & (ports - 1)) * Sequence::channel_count) + message.channel();
        if (part >= 0 && part < ChannelMask::channel_count) {
            seen[static_cast<std::size_t>(part)] = true;
        }
    }

    used_parts_.clear();
    for (int part = 0; part < ChannelMask::channel_count; ++part) {
        if (seen[static_cast<std::size_t>(part)]) {
            used_parts_.push_back(part);
        }
    }

    song_length_ = events.empty() ? 0 : events.back().position;
    song_name_ = std::move(name);
    song_events_ = std::move(events);

    // Commit the export planes now, while nothing is playing. Touching 64 MB of fresh pages takes
    // long enough to starve the pump's lead, and the first export used to do it mid-song; here the
    // pause is free. Kept for the life of the song and reused by every export of it.
    const auto total =
        static_cast<std::size_t>(song_length_ + static_cast<std::int64_t>(tail_seconds * sample_rate));
    export_left_.assign(total, 0.0f);
    export_right_.assign(total, 0.0f);
    export_wav_.reserve(total * 4 + 44);

    if (engine_) {
        engine_->reset();
        player_.emplace(*engine_, song_events_);
    }
}

void WebSession::unload_song()
{
    export_abort();
    export_left_ = {};
    export_right_ = {};
    export_wav_ = {};
    song_events_.clear();
    song_name_.clear();
    song_length_ = 0;
    used_parts_.clear();
    player_.reset();
    if (engine_) {
        engine_->reset();
    }
}

void WebSession::set_settings(const EngineSettings& settings)
{
    settings_ = settings;
    if (notes_) {
        rebuild();
    }
}

void WebSession::set_output_gain(double gain)
{
    output_gain_ = gain;
    if (engine_) {
        engine_->set_output_gain(gain);
    }
}

void WebSession::set_drum_map_row(std::optional<int> row)
{
    drum_map_row_ = row;
    if (engine_) {
        engine_->set_drum_map_row(row);
    }
}

int WebSession::effective_drum_map_row() const
{
    if (engine_) {
        return engine_->effective_drum_map_row();
    }
    if (drum_map_row_) {
        return *drum_map_row_;
    }
    return DrumKitTable::row_for_map(settings_.map).value_or(0);
}

void WebSession::seek(std::int64_t sample)
{
    if (player_) {
        player_->seek(std::max<std::int64_t>(0, sample));
    }
}

std::int64_t WebSession::position() const noexcept
{
    if (player_) {
        return player_->position();
    }
    if (engine_) {
        return engine_->position();
    }
    return 0;
}

bool WebSession::song_complete() const noexcept
{
    return player_ && has_song()
           && player_->position()
                  >= song_length_ + static_cast<std::int64_t>(tail_seconds * sample_rate);
}

void WebSession::panic()
{
    if (engine_) {
        engine_->reset();
    }
}

void WebSession::render_song(float* buffer, int frames)
{
    if (frames < 0 || frames > max_render_frames) {
        throw std::invalid_argument("render request outside the buffer's capacity");
    }

    std::span<float> left{buffer, static_cast<std::size_t>(frames)};
    std::span<float> right{buffer + frames, static_cast<std::size_t>(frames)};

    if (player_) {
        player_->render(left, right);
        return;
    }

    render_live(buffer, frames);
}

void WebSession::render_live(float* buffer, int frames)
{
    if (frames < 0 || frames > max_render_frames) {
        throw std::invalid_argument("render request outside the buffer's capacity");
    }

    std::span<float> left{buffer, static_cast<std::size_t>(frames)};
    std::span<float> right{buffer + frames, static_cast<std::size_t>(frames)};

    if (engine_) {
        engine_->render(left, right);
        return;
    }

    // The pump reuses its buffer, so leaving it untouched would queue the previous block again — a
    // stutter rather than the silence that is meant.
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
}

void WebSession::send_channel(int status, int data1, int data2)
{
    if (engine_) {
        engine_->send_channel(status, data1, data2);
    }
}

/// The kit each port has loaded, in port order.
///
/// One per port and not one overall: each port has its own drum part, so a two-port file can be
/// playing two different kits at once, and the strip that says which is the one on that port.
static json drum_kits_json(const ToneGenerator* engine)
{
    json kits = json::array();
    if (engine != nullptr) {
        for (int port = 0; port < engine->ports(); ++port) {
            kits.push_back(engine->drum_kit_for(port));
        }
    }
    return kits;
}

void WebSession::send_control(int part, int controller, int value)
{
    send_control(part / Sequence::channel_count, part % Sequence::channel_count, controller, value);
}

void WebSession::send_control(int port, int channel, int controller, int value)
{
    if (engine_) {
        engine_->send_channel(port, 0xB0 | (channel & 0x0F), controller, value);
    }
}

std::string WebSession::snapshot_json() const
{
    json channels = json::array();

    // Per-channel activity from the pool itself; there is no per-channel counter, and a walk over
    // at most 64 handles ten times a second costs nothing.
    std::array<int, ChannelMask::channel_count> counts{};
    if (engine_) {
        for (const auto& voice : engine_->voices().active()) {
            const int channel = voice.channel();
            if (channel >= 0 && channel < ChannelMask::channel_count) {
                ++counts[static_cast<std::size_t>(channel)];
            }
        }
    }

    // The engine's own part count, not the mask's. `ChannelMask` is sixty-four wide because a
    // four-port engine has that many parts to mute; this engine has as many as its ports give it,
    // and asking for one past them reads memory that is not a part.
    const int part_count = engine_ ? engine_->parts() : Sequence::channel_count;

    for (int channel = 0; channel < part_count; ++channel) {
        if (!engine_) {
            channels.push_back(json::object());
            continue;
        }

        const Part& part = engine_->part(channel);

        std::string name = nothing;
        if (notes_) {
            const auto& directory = notes_->directory();
            const int tone = directory.program_to_tone(part.program, settings_.map, part.bank);
            if (auto record = directory.tone(tone); tone >= 0 && record && record->is_defined()) {
                name = trimmed(record->name());
            }
        }

        channels.push_back(json{{"program", part.program},
                                {"bank", part.bank},
                                {"name", name},
                                {"volume", part.volume()},
                                {"pan", part.pan},
                                {"expression", part.expression()},
                                {"reverbSend", part.reverb_send},
                                {"chorusSend", part.chorus_send},
                                {"voices", counts[static_cast<std::size_t>(channel)]},
                                {"muted", channels_.is_muted(channel)},
                                {"soloed", channels_.is_soloed(channel)}});
    }

    const json snapshot{{"position", position()},
                        {"activeVoices", engine_ ? engine_->active_voices() : 0},
                        {"noteCount", engine_ ? engine_->note_count() : 0},
                        {"drumKit", engine_ ? engine_->drum_kit() : -1},
                        {"drumKits", drum_kits_json(engine_ ? &*engine_ : nullptr)},
                        {"effectiveDrumMapRow", effective_drum_map_row()},
                        {"songComplete", song_complete()},
                        {"channels", channels}};

    return snapshot.dump();
}

std::string WebSession::rom_info_json() const
{
    if (!rom_) {
        return "null";
    }

    return json{{"name", rom_name_},
                {"size", rom_length_},
                {"sha256", rom_->manifest().dll().sha256},
                {"verified", rom_verified_}}
        .dump();
}

std::string WebSession::song_info_json() const
{
    if (!has_song()) {
        return "null";
    }

    return json{{"name", song_name_},
                {"lengthSamples", song_length_},
                {"usedParts", used_parts_}}
        .dump();
}

std::string WebSession::vintage_catalog_json(ToneMap map) const
{
    if (!notes_) {
        return "null";
    }

    const auto& directory = notes_->directory();
    constexpr int program_count = 128;
    constexpr int bank_count = 128;

    // Bank 0 first and unconditionally: it is the capital bank every other bank falls back to, so
    // building it once spares re-resolving every hole in all 127 variations.
    std::array<CatalogEntry, program_count> capital;

    const auto build_bank = [&](int bank, std::array<CatalogEntry, program_count>& entries) {
        int native = 0;

        for (int program = 0; program < program_count; ++program) {
            const auto raw = directory.lut3_raw(program, map, bank);

            if (!raw || *raw == PatchDirectory::unassigned) {
                // Nothing native here. On a variation bank the module sounds bank 0's capital tone
                // rather than falling silent; on bank 0 itself there is nothing behind it.
                if (bank != 0 && capital[static_cast<std::size_t>(program)].kind
                                     == std::string_view("native")) {
                    entries[static_cast<std::size_t>(program)] =
                        capital[static_cast<std::size_t>(program)];
                    entries[static_cast<std::size_t>(program)].kind = "capitalFallback";
                } else {
                    entries[static_cast<std::size_t>(program)] = CatalogEntry{};
                }
                continue;
            }

            if (*raw >= PatchDirectory::indirect_only_flag) {
                entries[static_cast<std::size_t>(program)] =
                    CatalogEntry{-1, indirect_name(directory, *raw), "indirectOnly"};
                continue;
            }

            // program_tones is what the engine's note-on calls, so a slot counts as playable only
            // where the engine would find something to play.
            const auto tones = directory.program_tones(program, map, bank);
            const int tone = tones.empty() ? -1 : tones.front();

            if (auto record = directory.tone(tone); tone >= 0 && record && record->is_defined()) {
                entries[static_cast<std::size_t>(program)] =
                    CatalogEntry{tone, trimmed(record->name()), "native"};
                ++native;
                continue;
            }

            entries[static_cast<std::size_t>(program)] = CatalogEntry{};
        }

        return native;
    };

    json banks = json::array();
    std::set<int> tones;
    int native_total = 0;

    const auto append = [&](int bank, const std::array<CatalogEntry, program_count>& entries,
                            int native) {
        json programs = json::array();
        for (int program = 0; program < program_count; ++program) {
            const auto& entry = entries[static_cast<std::size_t>(program)];
            programs.push_back(entry_json(program, entry));
            if (entry.kind == std::string_view("native")) {
                tones.insert(entry.tone);
            }
        }
        banks.push_back(json{{"bank", bank}, {"nativeCount", native}, {"programs", programs}});
        native_total += native;
    };

    const int capital_native = build_bank(0, capital);
    append(0, capital, capital_native);

    std::array<CatalogEntry, program_count> entries;
    for (int bank = 1; bank < bank_count; ++bank) {
        const int native = build_bank(bank, entries);
        if (native == 0) {
            continue;
        }
        append(bank, entries, native);
    }

    return json{{"map", static_cast<int>(map)},
                {"nativeCount", native_total},
                {"toneCount", tones.size()},
                {"banks", banks}}
        .dump();
}

std::string WebSession::drum_catalog_json(int row) const
{
    if (!notes_) {
        return "null";
    }

    const auto& directory = notes_->directory();
    const auto& drums = notes_->drums();

    // Kits ordered by the first program that selects each, carrying every selecting program.
    std::vector<int> order;
    std::vector<std::pair<int, std::vector<int>>> programs;

    for (int program = 0; program < 128; ++program) {
        const auto kit = drums.kit_for_program(program, row);
        if (!kit) {
            continue;
        }

        auto found = std::find_if(programs.begin(), programs.end(),
                                  [&](const auto& entry) { return entry.first == *kit; });
        if (found == programs.end()) {
            programs.emplace_back(*kit, std::vector<int>{program});
        } else {
            found->second.push_back(program);
        }
    }

    json kits = json::array();
    for (const auto& [kit, selectors] : programs) {
        json keys = json::array();

        // Drum sounds are ordinary melodic tones, so a key is named from the same table a program
        // change names an instrument from.
        for (int note = 0; note < DrumKitTable::key_count; ++note) {
            const DrumKey key = drums.key(note, kit);
            const auto tone = directory.tone(key.tone);
            if (!tone || !tone->is_defined()) {
                continue;
            }

            keys.push_back(json{{"note", note},
                                {"tone", key.tone},
                                {"name", trimmed(tone->name())},
                                {"level", key.level},
                                {"pitch", key.pitch},
                                {"group", key.group},
                                {"pan", key.pan}});
        }

        kits.push_back(json{{"kit", kit}, {"programs", selectors}, {"keys", keys}});
    }

    return json{{"row", row}, {"kits", kits}}.dump();
}

void WebSession::export_begin()
{
    if (!notes_ || !has_song()) {
        throw std::runtime_error("Load a DLL and a song before exporting.");
    }

    export_abort();

    // The drum map row goes with it: an export that resolved kits through a different map than the
    // one being listened to would be a different arrangement, not a rendering of this one.
    export_engine_.emplace(*notes_, options());
    export_engine_->set_drum_map_row(drum_map_row_);
    export_engine_->set_output_gain(output_gain_);
    export_player_.emplace(*export_engine_, song_events_);

    export_total_ = song_length_ + static_cast<std::int64_t>(tail_seconds * sample_rate);
    export_rendered_ = 0;
    export_left_.assign(static_cast<std::size_t>(export_total_), 0.0f);
    export_right_.assign(static_cast<std::size_t>(export_total_), 0.0f);
    export_wav_.clear();
}

double WebSession::export_step()
{
    if (!export_player_) {
        throw std::runtime_error("No export in progress.");
    }

    // A quarter of a second at a time: long enough that the yield back to the worker's queue costs
    // nothing measurable, short enough that queue reports keep interleaving.
    constexpr std::int64_t chunk = sample_rate / 4;

    const auto count = std::min(chunk, export_total_ - export_rendered_);
    if (count > 0) {
        const auto start = static_cast<std::size_t>(export_rendered_);
        export_player_->render(
            std::span<float>{export_left_.data() + start, static_cast<std::size_t>(count)},
            std::span<float>{export_right_.data() + start, static_cast<std::size_t>(count)});
        export_rendered_ += count;
    }

    if (export_rendered_ < export_total_) {
        return static_cast<double>(export_rendered_) / static_cast<double>(export_total_);
    }

    // Through the library's own writer via MEMFS, so a browser export and a CLI render with the
    // same settings are the same bytes, not merely similar ones.
    const char* path = "/export.wav";
    wav::write(path, export_left_, export_right_, sample_rate);

    std::ifstream file{path, std::ios::binary};
    export_wav_.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    file.close();
    std::remove(path);

    export_engine_.reset();
    export_player_.reset();

    return 1.0;
}

// The planes survive an abort on purpose: they belong to the loaded song, and freeing them here
// would put the first-touch cost back into the next export. unload_song is where they go.
void WebSession::export_abort()
{
    export_player_.reset();
    export_engine_.reset();
    export_total_ = 0;
    export_rendered_ = 0;
}

ToneGeneratorOptions WebSession::options() const
{
    ToneGeneratorOptions options;
    options.map = settings_.map;
    options.reverb = settings_.reverb;
    options.chorus = settings_.chorus;
    options.delay = settings_.delay;
    options.output_gain = output_gain_;
    options.channels = &channels_;
    return options;
}

void WebSession::rebuild()
{
    const std::int64_t position = player_ ? player_->position() : 0;

    // A rebuild makes fresh parts at their power-on values; capture the outgoing ones first so a
    // vintage change does not silently reset every channel to piano.
    std::vector<std::array<int, 7>> previous;
    if (engine_) {
        previous.reserve(static_cast<std::size_t>(engine_->parts()));
        for (int channel = 0; channel < engine_->parts(); ++channel) {
            const Part& part = engine_->part(channel);
            previous.push_back({part.bank, part.program, part.volume(), part.pan,
                                part.expression(), part.reverb_send, part.chorus_send});
        }
    }

    // The player holds a pointer into the outgoing engine, so it goes first.
    player_.reset();
    engine_.emplace(*notes_, options());
    engine_->set_drum_map_row(drum_map_row_);

    if (!previous.empty()) {
        restore_parts(previous);
    }

    if (has_song()) {
        player_.emplace(*engine_, song_events_);

        // Put the new generator back where the old one was, so changing vintage mid-song resumes
        // rather than restarting. Seek replays the controllers, which is what makes that sound
        // right.
        if (position > 0) {
            player_->seek(position);
        }
    }
}

/// Carries the parts' settings across a rebuild by sending the messages that made them, so the new
/// parts are configured through the same path a controller or a file uses — the drum kit comes back
/// with the program change, and nothing here has to know what else a program change does. Voices
/// are still lost; only the settings survive.
void WebSession::restore_parts(const std::vector<std::array<int, 7>>& previous)
{
    for (int index = 0; index < static_cast<int>(previous.size()); ++index) {
        const auto& [bank, program, volume, pan, expression, reverb, chorus] = previous
            [static_cast<std::size_t>(index)];

        // A part index is not a channel. `port * 16 + channel` has to be taken apart again before
        // it can go back out as MIDI, because a status byte carries four bits of channel and the
        // port travels beside it -- `0xC0 | 16` is not part 17's program change, it is a channel
        // aftertouch on part 1.
        const int port = index / Sequence::channel_count;
        const int channel = index % Sequence::channel_count;

        // Bank before program, as anything selecting a sound must: the program change is what
        // latches the pair.
        send_control(port, channel, 0, bank);
        engine_->send_channel(port, 0xC0 | channel, program, 0);

        send_control(port, channel, 7, volume);
        send_control(port, channel, 10, pan);
        send_control(port, channel, 11, expression);
        send_control(port, channel, 91, reverb);
        send_control(port, channel, 93, chorus);
    }
}

} // namespace ts::web
