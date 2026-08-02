#include "tabulasonora/note_renderer.hpp"
#include "tabulasonora/rom_image.hpp"
#include "tabulasonora/table_manifest.hpp"
#include "tabulasonora/wave_rom.hpp"

#include <CLI/CLI.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"A native implementation of the Roland Sound Canvas VA synth voice.",
                 "tabula-sonora"};
    app.require_subcommand(1);

    CLI::App* manifest = app.add_subcommand("manifest", "Show the pinned DLL build and table map.");

    std::string dll_path;
    CLI::App* info = app.add_subcommand("info", "Verify an SCCore.dll and describe it.");
    info->add_option("dll", dll_path, "Path to SCCore.dll")->required();

    fs::path output_directory;
    CLI::App* extract =
        app.add_subcommand("extract-tables", "Write every static table out as a .bin slice.");
    extract->add_option("dll", dll_path, "Path to SCCore.dll")->required();
    extract->add_option("output", output_directory, "Directory to write into")->required();

    int program = 0;
    int note = 60;
    int velocity = 100;
    double hold_seconds = 1.0;
    int map = 4;
    fs::path output_file;
    CLI::App* render_note =
        app.add_subcommand("render-note", "Render one note to raw interleaved float32.");
    render_note->add_option("dll", dll_path, "Path to SCCore.dll")->required();
    render_note->add_option("program", program, "Program number, 0-127")->required();
    render_note->add_option("note", note, "MIDI note, 0-127")->required();
    render_note->add_option("velocity", velocity, "MIDI velocity, 1-127")->required();
    render_note->add_option("hold", hold_seconds, "Seconds from note-on to note-off")->required();
    render_note->add_option("output", output_file, "Output .f32 path")->required();
    render_note->add_option("map", map, "Tone map: 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820");

    CLI11_PARSE(app, argc, argv);

    try {
        if (manifest->parsed()) {
            return manifest_command();
        }
        if (info->parsed()) {
            return info_command(dll_path);
        }
        if (extract->parsed()) {
            return extract_tables_command(dll_path, output_directory);
        }
        if (render_note->parsed()) {
            return render_note_command(
                dll_path, program, note, velocity, hold_seconds, output_file, map);
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
