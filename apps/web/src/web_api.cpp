// The extern "C" boundary the worker drives through cwrap.
//
// One session per worker, so free functions over a singleton; every C++ exception is caught here
// and surfaced as a return code plus ts_web_last_error(), because an exception crossing into JS
// aborts the module. Strings are returned as pointers into module-owned storage that stays valid
// until the next call — the JS side copies them out immediately.

#include "web_session.hpp"

#include <emscripten/emscripten.h>

#include <array>
#include <cstdint>
#include <exception>
#include <string>

namespace {

ts::web::WebSession& session()
{
    static ts::web::WebSession instance;
    return instance;
}

std::string& last_error()
{
    static std::string error;
    return error;
}

std::string& text_result()
{
    static std::string result;
    return result;
}

/// The render target: two planes of up to max_render_frames floats, left then right, packed to the
/// requested frame count — exactly the layout one worklet push takes.
std::array<float, 2 * ts::web::WebSession::max_render_frames>& render_buffer()
{
    static std::array<float, 2 * ts::web::WebSession::max_render_frames> buffer;
    return buffer;
}

int guarded(const char* context, auto&& action)
{
    try {
        action();
        return 0;
    } catch (const std::exception& error) {
        last_error() = std::string{context} + ": " + error.what();
        return -1;
    }
}

const char* guarded_text(const char* context, auto&& action)
{
    try {
        text_result() = action();
        return text_result().c_str();
    } catch (const std::exception& error) {
        last_error() = std::string{context} + ": " + error.what();
        return nullptr;
    }
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* ts_web_last_error()
{
    return last_error().c_str();
}

EMSCRIPTEN_KEEPALIVE int ts_web_sample_rate()
{
    return ts::web::WebSession::sample_rate;
}

EMSCRIPTEN_KEEPALIVE int ts_web_max_render_frames()
{
    return ts::web::WebSession::max_render_frames;
}

EMSCRIPTEN_KEEPALIVE int ts_web_drum_channel()
{
    return ts::web::WebSession::drum_channel();
}

EMSCRIPTEN_KEEPALIVE int ts_web_load_rom(std::uint8_t* data, int length, const char* name,
                                         const char* expected_sha256)
{
    return guarded("load_rom", [&] {
        session().load_rom(data, static_cast<std::size_t>(length), name ? name : "<memory>",
                           expected_sha256 ? expected_sha256 : "");
    });
}

EMSCRIPTEN_KEEPALIVE void ts_web_unload_rom()
{
    session().unload_rom();
}

EMSCRIPTEN_KEEPALIVE const char* ts_web_rom_info_json()
{
    return guarded_text("rom_info", [&] { return session().rom_info_json(); });
}

EMSCRIPTEN_KEEPALIVE int ts_web_load_song(const std::uint8_t* data, int length, const char* name)
{
    return guarded("load_song", [&] {
        session().load_song({data, static_cast<std::size_t>(length)}, name ? name : "<memory>");
    });
}

EMSCRIPTEN_KEEPALIVE void ts_web_unload_song()
{
    session().unload_song();
}

EMSCRIPTEN_KEEPALIVE const char* ts_web_song_info_json()
{
    return guarded_text("song_info", [&] { return session().song_info_json(); });
}

EMSCRIPTEN_KEEPALIVE int ts_web_set_settings(int map, int reverb, int chorus, int delay, int efx)
{
    return guarded("set_settings", [&] {
        session().set_settings({static_cast<ts::ToneMap>(map), reverb != 0, chorus != 0,
                                delay != 0, efx != 0});
    });
}

EMSCRIPTEN_KEEPALIVE void ts_web_set_output_gain(double gain)
{
    session().set_output_gain(gain);
}

EMSCRIPTEN_KEEPALIVE void ts_web_set_drum_map_row(int row)
{
    session().set_drum_map_row(row < 0 ? std::nullopt : std::optional<int>{row});
}

EMSCRIPTEN_KEEPALIVE void ts_web_set_looping(int looping)
{
    session().set_looping(looping != 0);
}

EMSCRIPTEN_KEEPALIVE int ts_web_effective_drum_map_row()
{
    return session().effective_drum_map_row();
}

EMSCRIPTEN_KEEPALIVE void ts_web_seek(double sample)
{
    session().seek(static_cast<std::int64_t>(sample));
}

EMSCRIPTEN_KEEPALIVE void ts_web_panic()
{
    session().panic();
}

EMSCRIPTEN_KEEPALIVE int ts_web_song_complete()
{
    return session().song_complete() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE double ts_web_position()
{
    return static_cast<double>(session().position());
}

EMSCRIPTEN_KEEPALIVE float* ts_web_render_buffer()
{
    return render_buffer().data();
}

EMSCRIPTEN_KEEPALIVE int ts_web_render_song(int frames)
{
    return guarded("render_song", [&] { session().render_song(render_buffer().data(), frames); });
}

EMSCRIPTEN_KEEPALIVE int ts_web_render_live(int frames)
{
    return guarded("render_live", [&] { session().render_live(render_buffer().data(), frames); });
}

EMSCRIPTEN_KEEPALIVE void ts_web_send_channel(int status, int data1, int data2)
{
    session().send_channel(status, data1, data2);
}

EMSCRIPTEN_KEEPALIVE void ts_web_send_control(int channel, int controller, int value)
{
    session().send_control(channel, controller, value);
}

EMSCRIPTEN_KEEPALIVE void ts_web_set_muted(int channel, int muted)
{
    session().set_muted(channel, muted != 0);
}

EMSCRIPTEN_KEEPALIVE void ts_web_set_soloed(int channel, int soloed)
{
    session().set_soloed(channel, soloed != 0);
}

EMSCRIPTEN_KEEPALIVE void ts_web_channels_reset()
{
    session().channels_reset();
}

EMSCRIPTEN_KEEPALIVE const char* ts_web_snapshot_json()
{
    return guarded_text("snapshot", [&] { return session().snapshot_json(); });
}

EMSCRIPTEN_KEEPALIVE const char* ts_web_vintage_catalog_json(int map)
{
    return guarded_text("vintage_catalog", [&] {
        return session().vintage_catalog_json(static_cast<ts::ToneMap>(map));
    });
}

EMSCRIPTEN_KEEPALIVE const char* ts_web_drum_catalog_json(int row)
{
    return guarded_text("drum_catalog", [&] { return session().drum_catalog_json(row); });
}

EMSCRIPTEN_KEEPALIVE int ts_web_export_begin()
{
    return guarded("export_begin", [&] { session().export_begin(); });
}

EMSCRIPTEN_KEEPALIVE double ts_web_export_step()
{
    try {
        return session().export_step();
    } catch (const std::exception& error) {
        last_error() = std::string{"export_step: "} + error.what();
        return -1.0;
    }
}

EMSCRIPTEN_KEEPALIVE const std::uint8_t* ts_web_export_bytes()
{
    return session().export_bytes().data();
}

EMSCRIPTEN_KEEPALIVE int ts_web_export_length()
{
    return static_cast<int>(session().export_bytes().size());
}

EMSCRIPTEN_KEEPALIVE void ts_web_export_abort()
{
    session().export_abort();
}

} // extern "C"
