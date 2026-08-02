#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

/// Locating the Roland-derived assets the conformance tests need.
///
/// Nothing Roland-derived is committed, so every asset here may legitimately be absent. Tests that
/// need one skip with an actionable message rather than failing: a clone with no DLL still builds
/// and still runs the pure-logic tests, which is the property that keeps the suite useful to
/// somebody who has not bought Sound Canvas VA.
namespace ts::testdata {

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

/// SHA-256, as lower-case hex, of a run of 32-bit values serialised little-endian.
///
/// Comparing a whole predictor stream literally would mean a fixture the size of the wave ROM, so
/// the differential fixtures record a digest instead. Little-endian because that is what the
/// generator writes, and the two have to agree byte for byte or the digest means nothing.
[[nodiscard]] std::string sha256_of_le32(std::span<const std::int32_t> values);

} // namespace ts::testdata
