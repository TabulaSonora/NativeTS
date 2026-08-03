#pragma once

#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/sequence_player.hpp"
#include "tabulasonora/render_options.hpp"
#include "tabulasonora/smf_reader.hpp"
#include "tabulasonora/tone_generator.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts::web {

/// The engine settings the page remembers between visits: exactly the values a rebuild depends on,
/// held as one unit so restoring a remembered set costs one rebuild rather than four.
struct EngineSettings {
    ToneMap map = ToneMap::sc8820;
    bool reverb = true;
    bool chorus = true;
    bool delay = true;
};

/// One running engine and everything the page can do to it — the worker-side counterpart of the
/// reference web app's SynthSession.
///
/// Holds the objects in the order the library builds them: the ROM bytes, a RomImage over them, one
/// NoteRenderer that loads the tables, and a ToneGenerator over that. Changing a setting that lives
/// in ToneGeneratorOptions rebuilds the generator but *not* the note renderer, so switching vintage
/// or effects costs nothing but the voices that were sounding — the 27 MB of tables are read once
/// per session.
///
/// Not thread-safe, and does not need to be: the module lives in one dedicated worker, so the pump,
/// the forwarded UI events and the live MIDI all arrive on the same thread, which is exactly the
/// contract ToneGenerator asks for.
class WebSession {
public:
    static constexpr int sample_rate = ToneGenerator::sample_rate;

    /// Largest render request one call accepts, in frames.
    static constexpr int max_render_frames = 4096;

    /// Seconds of release and effect tail rendered past the last event.
    static constexpr double tail_seconds = 2.2;

    /// The channel routed to the drum path, read from the engine's own default.
    static int drum_channel() { return ToneGeneratorOptions{}.drum_channel; }

    /// Takes ownership of malloc'd DLL bytes and builds the engine over them.
    ///
    /// With a 64-hex-digit `expected_sha256` from storage the image is verified by size and PE
    /// timestamp only — but the stored hash still has to match the build the library is pinned to;
    /// the quick check skips recomputing it, not believing it.
    ///
    /// Throws RomIdentityError if the bytes are not the pinned build.
    void load_rom(std::uint8_t* data, std::size_t length, std::string name,
                  const std::string& expected_sha256);

    /// Forgets everything, releasing the ROM bytes.
    void unload_rom();

    /// Parses a Standard MIDI File and arms it for playback.
    void load_song(std::span<const std::uint8_t> midi, std::string name);

    /// Forgets the loaded song, leaving the engine up for live playing.
    void unload_song();

    [[nodiscard]] bool has_rom() const noexcept { return engine_.has_value(); }
    [[nodiscard]] bool has_song() const noexcept { return !song_name_.empty(); }

    /// All four engine settings at once: one rebuild, with part state carried across it.
    void set_settings(const EngineSettings& settings);
    [[nodiscard]] const EngineSettings& settings() const noexcept { return settings_; }

    /// Linear gain on the audio handed to the browser; live, no rebuild.
    void set_output_gain(double gain);

    /// Which drum map row a program change on the drum part resolves against; empty follows the
    /// vintage. Survives rebuilds, exactly because a rebuild has nothing to do with it.
    void set_drum_map_row(std::optional<int> row);
    [[nodiscard]] int effective_drum_map_row() const;

    /// Jumps the song to a position, replaying the controllers up to it.
    void seek(std::int64_t sample);

    [[nodiscard]] std::int64_t position() const noexcept;

    /// Whether the song has been rendered past its end plus the tail.
    [[nodiscard]] bool song_complete() const noexcept;

    /// Silences everything and returns every part to its power-on state.
    void panic();

    /// Renders the next block of the loaded song into two planes of `frames` floats each, at
    /// `buffer` and `buffer + frames` — the layout the worklet ring takes. Live notes still sound
    /// over a running song, since one generator serves both.
    void render_song(float* buffer, int frames);

    /// Renders from the generator alone, without advancing any song — live playing, including when
    /// a song is loaded but stopped.
    void render_live(float* buffer, int frames);

    /// One channel voice message on port A — live MIDI and the on-screen keyboard.
    void send_channel(int status, int data1, int data2);

    /// One Control Change — the mixer's faders. Really sent as MIDI, so a running sequence
    /// overwrites it at its next controller event, exactly as on the module's front panel.
    void send_control(int channel, int controller, int value);

    /// Per-channel mute and solo. Applied at the mix where no MIDI message reaches, and shared with
    /// every generator this session builds, so they survive rebuilds.
    void set_muted(int channel, bool muted) { channels_.set_muted(channel, muted); }
    void set_soloed(int channel, bool soloed) { channels_.set_soloed(channel, soloed); }
    void channels_reset() { channels_.reset(); }

    /// Everything the 10 Hz redraw needs, as one JSON document.
    [[nodiscard]] std::string snapshot_json() const;

    [[nodiscard]] std::string rom_info_json() const;
    [[nodiscard]] std::string song_info_json() const;

    /// Every bank and program one vintage defines — the reference SoundCatalog sweep.
    [[nodiscard]] std::string vintage_catalog_json(ToneMap map) const;

    /// Every kit one drum map row reaches.
    [[nodiscard]] std::string drum_catalog_json(int row) const;

    /// Starts a WAV export through a *second* generator over the same note renderer, so exporting
    /// disturbs nothing that is playing and costs no second read of the tables.
    void export_begin();

    /// Renders the next quarter-second; returns progress in [0, 1], 1.0 when the file is ready.
    [[nodiscard]] double export_step();

    [[nodiscard]] std::span<const std::uint8_t> export_bytes() const noexcept { return export_wav_; }

    void export_abort();

private:
    struct FreeDeleter {
        void operator()(std::uint8_t* p) const noexcept { std::free(p); }
    };

    [[nodiscard]] ToneGeneratorOptions options() const;
    void rebuild();
    void restore_parts(const std::vector<std::array<int, 7>>& previous);

    std::unique_ptr<std::uint8_t[], FreeDeleter> rom_bytes_;
    std::size_t rom_length_ = 0;
    std::optional<RomImage> rom_;
    std::optional<NoteRenderer> notes_;
    std::optional<ToneGenerator> engine_;
    std::optional<SequencePlayer> player_;

    std::string rom_name_;
    bool rom_verified_ = false;

    std::vector<MidiEvent> song_events_;
    std::string song_name_;
    std::int64_t song_length_ = 0;
    int used_channels_ = 0;

    EngineSettings settings_;
    std::optional<int> drum_map_row_;
    double output_gain_ = 1.0;
    ChannelMask channels_;

    // Export state, alive only between export_begin and the last export_step.
    std::optional<ToneGenerator> export_engine_;
    std::optional<SequencePlayer> export_player_;
    std::vector<float> export_left_;
    std::vector<float> export_right_;
    std::int64_t export_total_ = 0;
    std::int64_t export_rendered_ = 0;
    std::vector<std::uint8_t> export_wav_;
};

} // namespace ts::web
