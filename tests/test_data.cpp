#include "test_data.hpp"

#include "rom/sha256.hpp"

#include <nlohmann/json.hpp>
#include "tabulasonora/effect_presets.hpp"
#include "tabulasonora/rom_image.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef TS_REPOSITORY_ROOT
#    define TS_REPOSITORY_ROOT "."
#endif

namespace ts::testdata {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::optional<fs::path> from_environment(const char* variable)
{
    const char* value = std::getenv(variable);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }

    fs::path path{value};
    std::error_code error;
    if (fs::exists(path, error)) {
        return path;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<fs::path> first_existing(const std::vector<fs::path>& candidates)
{
    for (const fs::path& candidate : candidates) {
        std::error_code error;
        if (fs::exists(candidate, error)) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace

const fs::path& repository_root()
{
    static const fs::path root{TS_REPOSITORY_ROOT};
    return root;
}

const RomImage& shared_rom()
{
    // `require_sccore` skips the test when the DLL is absent, so this runs only where there is one
    // to open. Function-local static: the standard makes the initialisation itself thread-safe, and
    // a second thread reaching here while the first is still reading waits rather than racing.
    static const RomImage image =
        RomImage::open(require_sccore().string(), RomVerification::quick);

    // Computed from this image, once, and behind the presets' own mutex. Done here rather than left
    // to whichever worker constructs its renderer first, so the lazy path is never the thing being
    // raced on.
    EffectPresets::ensure_from(image);
    return image;
}

unsigned worker_count()
{
    static const unsigned count = [] {
        if (const char* requested = std::getenv("TS_TEST_THREADS")) {
            return static_cast<unsigned>(std::max(1, std::atoi(requested)));
        }
        return std::max(1U, std::thread::hardware_concurrency());
    }();
    return count;
}

void parallel_for(std::size_t count, const std::function<void(std::size_t)>& body)
{
    if (count == 0) {
        return;
    }

    const auto workers =
        static_cast<std::size_t>(std::min<std::size_t>(worker_count(), count));
    if (workers <= 1) {
        // No thread at all rather than one worker thread: a sanitiser run or a debugger session
        // asking for one thread wants the work on the stack it is already watching.
        for (std::size_t index = 0; index < count; ++index) {
            body(index);
        }
        return;
    }

    // A shared cursor rather than a contiguous slice each: the sweeps this serves are wildly uneven
    // -- one song can render for a minute and the next for a second -- and an even split would end
    // with every worker but one already finished.
    std::atomic<std::size_t> cursor{0};
    std::mutex failure_gate;
    std::exception_ptr failure;

    const auto run = [&] {
        for (;;) {
            const std::size_t index = cursor.fetch_add(1);
            if (index >= count) {
                return;
            }
            try {
                body(index);
            } catch (...) {
                // Recorded and re-thrown on the caller's thread. Letting it leave a worker would
                // terminate the process and take the rest of the suite's results with it.
                const std::lock_guard<std::mutex> guard{failure_gate};
                if (!failure) {
                    failure = std::current_exception();
                }
                return;
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(workers - 1);
    for (std::size_t i = 1; i < workers; ++i) {
        pool.emplace_back(run);
    }
    run(); // The caller's thread is one of the workers.
    for (std::thread& worker : pool) {
        worker.join();
    }

    if (failure) {
        std::rethrow_exception(failure);
    }
}

std::string sha256_of_le32(std::span<const std::int32_t> values)
{
    Sha256 hash;

    std::array<std::uint8_t, 4> bytes{};
    for (std::int32_t value : values) {
        const auto word = static_cast<std::uint32_t>(value);
        bytes[0] = static_cast<std::uint8_t>(word);
        bytes[1] = static_cast<std::uint8_t>(word >> 8);
        bytes[2] = static_cast<std::uint8_t>(word >> 16);
        bytes[3] = static_cast<std::uint8_t>(word >> 24);
        hash.update(bytes.data(), bytes.size());
    }

    return hash.finish_hex();
}

std::optional<fs::path> sccore()
{
    if (auto path = from_environment("TS_SCCORE")) {
        return path;
    }

    return first_existing({
        repository_root() / "SCCore.dll",
        // The sibling C# checkout this engine is ported from and checked against.
        repository_root().parent_path() / "DotNetAdministravit" / "SCCore.dll",
        "C:/Program Files/Roland VS/SOUND Canvas VA/SCCore.dll",
    });
}

std::optional<fs::path> tables()
{
    if (auto path = from_environment("TS_TABLES")) {
        return path;
    }

    return first_existing({
        repository_root() / "tables",
        repository_root().parent_path() / "DotNetAdministravit" / "tables",
    });
}

fs::path require_sccore()
{
    auto path = sccore();
    if (!path) {
        SKIP("SCCore.dll not found. Set TS_SCCORE, or place the SOUND Canvas VA 1.1.6 build at the "
             "repository root. Nothing Roland-derived is committed.");
    }
    return *path;
}

fs::path require_tables()
{
    auto directory = tables();
    if (!directory) {
        SKIP("No extracted tables found. Set TS_TABLES, or run "
             "'tabula-sonora extract-tables <SCCore.dll> tables/'.");
    }
    return *directory;
}

void require_effect_presets()
{
    if (!EffectPresets::available()) {
        if (const auto path = sccore()) {
            const RomImage rom = RomImage::open(path->string(), RomVerification::quick);
            EffectPresets::ensure_from(rom);
        }
    }

    if (!EffectPresets::available()) {
        SKIP("Effect presets unavailable: no SCCore.dll to compute them from and nothing pinned "
             "through TABULASONORA_PRESETS.");
    }
}

void require_current_fixture(const std::filesystem::path& fixture)
{
    namespace fs = std::filesystem;
    const fs::path manifest_path = fixture.parent_path() / "fixture_manifest.json";
    const std::string name = fixture.filename().string();

    const std::string how = "Regenerate with:\n"
                            "  python3 tools/dump_song_renders_oracle.py <SCCore.dll> "
                            "fixtures/song_renders_oracle.json --scdec <path to scdec>\n"
                            "  python3 tools/dump_note_renders_oracle.py <SCCore.dll> "
                            "fixtures/note_renders_oracle.json --scdec <path to scdec>\n"
                            "Each generator rewrites its own entry in fixture_manifest.json.";

    if (!fs::exists(manifest_path)) {
        SKIP("No fixture_manifest.json beside '" + name
             + "'. Fixtures are gitignored, so one that predates this check cannot be told apart "
               "from one left over from an older harness -- both are treated as needing "
               "regeneration.\n"
             + how);
    }

    std::ifstream stream{manifest_path};
    if (!stream) {
        SKIP("Cannot read '" + manifest_path.string() + "'.\n" + how);
    }

    nlohmann::json manifest;
    try {
        manifest = nlohmann::json::parse(stream);
    } catch (const std::exception& error) {
        SKIP("'" + manifest_path.string() + "' is not readable JSON (" + error.what() + ").\n"
             + how);
    }

    // Keyed on the repository-relative path the generator recorded, so a manifest is portable
    // between machines; the fixture's own name is the fallback for anything written differently.
    const auto& fixtures = manifest.value("fixtures", nlohmann::json::object());
    const std::string relative = "fixtures/" + name;
    const nlohmann::json* entry = nullptr;
    if (fixtures.contains(relative)) {
        entry = &fixtures.at(relative);
    } else if (fixtures.contains(name)) {
        entry = &fixtures.at(name);
    }
    if (entry == nullptr) {
        SKIP("'" + name + "' has no entry in fixture_manifest.json, so it was not written by the "
             "last regeneration.\n" + how);
    }

    Sha256 hash;
    std::ifstream source{fixture, std::ios::binary};
    if (!source) {
        SKIP("Cannot read '" + fixture.string() + "'.\n" + how);
    }
    std::vector<char> buffer(1u << 20);
    while (source) {
        source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto moved = static_cast<std::size_t>(source.gcount());
        if (moved == 0) {
            break;
        }
        hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), moved);
    }

    const std::string actual = hash.finish_hex();
    const auto expected = entry->value("sha256", std::string{});
    if (actual != expected) {
        SKIP("'" + name + "' does not match the hash the last regeneration recorded.\n"
             "  recorded " + expected + "\n"
             "  on disk  " + actual + "\n"
             "It has been modified, or it is left over from a different harness build. Judging a "
             "port against a reference nobody can reproduce is worse than not judging it.\n"
             + how);
    }
}

} // namespace ts::testdata
