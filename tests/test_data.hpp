#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

/// Locating the Roland-derived assets the conformance tests need.
///
/// Nothing Roland-derived is committed, so every asset here may legitimately be absent. Tests that
/// need one skip with an actionable message rather than failing: a clone with no DLL still builds
/// and still runs the pure-logic tests, which is the property that keeps the suite useful to
/// somebody who has not bought Sound Canvas VA.
namespace ts {

class RomImage;

namespace testdata {

/// The pinned `SCCore.dll`, if it can be found.
///
/// Resolved from `TS_SCCORE` first, then a candidate list including the sibling C# checkout.
[[nodiscard]] std::optional<std::filesystem::path> sccore();

/// The directory of extracted `tables/*.bin` slices, if it can be found.
///
/// Resolved from `TS_TABLES` first, then `tables/` at the repository root. Regenerate it with
/// `tabula-sonora extract-tables <SCCore.dll> tables/`.
[[nodiscard]] std::optional<std::filesystem::path> tables();

/// Skips the current test unless the DLL is present. Returns its path when it is.
[[nodiscard]] std::filesystem::path require_sccore();

/// Skips the current test unless the extracted tables are present. Returns the directory.
[[nodiscard]] std::filesystem::path require_tables();

/// Skips the current test unless effect presets can be had, computing them from the DLL when it is
/// present — the same wiring a `NoteRenderer` performs on construction.
///
/// Needed because the presets are no longer a compiled-in file: a test that reaches for them
/// without ever opening a ROM would otherwise depend on some earlier test having opened one.
void require_effect_presets();

/// The repository root, as baked in at configure time. Fixtures are resolved relative to it.
[[nodiscard]] const std::filesystem::path& repository_root();

/// Verifies a fixture against `fixtures/fixture_manifest.json`, and SKIPs the case if it cannot.
///
/// Fixtures are gitignored, so nothing in the tree says what state they are in. A harness change
/// that moves the reference renders leaves every existing copy silently wrong: the gate still runs,
/// still prints numbers, and measures them against something that no longer exists. That happened
/// twice on 2026-08-08 -- once when `scdec` stopped truncating event times, and again when it began
/// passing real `deltaFrames`, which moved the references by 0.32 peak at 0.16 correlation.
///
/// So the generators record a hash per fixture and this checks it. A missing manifest means the
/// fixtures predate the check, which is indistinguishable from stale and is treated as stale.
///
/// This proves the file is the one the last regeneration wrote. It cannot prove that regenerating
/// again would write the same bytes -- that needs the harness, which the gate does not have a path
/// to. The manifest records the harness's own hash for whoever is reading it after a surprise.
void require_current_fixture(const std::filesystem::path& fixture);

/// The one `RomImage` for the whole process, opened on first use.
///
/// Skips the current test if the DLL is absent, exactly as `require_sccore` does.
///
/// It is the only object here that is shared rather than made per worker. An image is immutable
/// once opened -- it owns the bytes and hands out spans into them -- so any number of threads may
/// read it at once. Everything downstream of it is not: a `NoteRenderer` carries the engine's noise
/// source and a `ToneGenerator` its voice pool, both of which a render mutates, so a worker builds
/// its own from this and never shares one.
///
/// Opening it once also takes the 27 MB read and its verification out of the per-test cost, which
/// is what makes the small gates cheap enough that only the sweeps are worth threading at all.
[[nodiscard]] const RomImage& shared_rom();

/// Worker threads a sweep gate should use, from `TS_TEST_THREADS`.
///
/// Defaults to the hardware's concurrency; 1 runs the sweep on the calling thread with no threads
/// spawned at all, which is what a debugger or a sanitiser wants. Values below 1 are clamped up.
///
/// Set it deliberately on a memory-tight machine: a worker holds a whole song's render in float
/// stereo while it measures it, so peak footprint scales with this and not with the corpus.
[[nodiscard]] unsigned worker_count();

/// Runs `body(index)` for every index in `[0, count)`, spread across `worker_count()` threads.
///
/// **Catch2's assertion macros are not thread-safe**, and neither is `INFO`. Nothing inside `body`
/// may assert: a sweep collects its outcome per index into a vector the caller sized up front --
/// one slot per index, so no locking is needed -- and asserts over that vector afterwards, on the
/// thread Catch2 is expecting. That ordering is also what keeps a failure reported against the case
/// that caused it rather than against whichever worker happened to reach it first.
///
/// An exception escaping `body` is re-thrown on the calling thread once every worker has finished,
/// so a fixture that throws still fails the test rather than calling `std::terminate`.
void parallel_for(std::size_t count, const std::function<void(std::size_t)>& body);

/// SHA-256, as lower-case hex, of a run of 32-bit values serialised little-endian.
///
/// Comparing a whole predictor stream literally would mean a fixture the size of the wave ROM, so
/// the differential fixtures record a digest instead. Little-endian because that is what the
/// generator writes, and the two have to agree byte for byte or the digest means nothing.
[[nodiscard]] std::string sha256_of_le32(std::span<const std::int32_t> values);

} // namespace testdata
} // namespace ts
