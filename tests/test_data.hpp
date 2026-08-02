#pragma once

#include <filesystem>
#include <optional>

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

} // namespace ts::testdata
