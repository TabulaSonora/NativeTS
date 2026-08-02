#pragma once

#include <string_view>

/// Data files compiled into the library.
///
/// The offset map is original work and ships with the engine; the data it points at does not. See
/// the upstream NOTICE.md for what remains Roland's and must be supplied from your own install.
namespace ts::assets {

/// The table offset map — `manifest.json`, the pinned DLL identity plus every table's byte-exact
/// location. Embedded so the engine is self-describing with no data files on disk.
[[nodiscard]] std::string_view manifest_json() noexcept;

/// The reverb, chorus and delay coefficients — `presets.json`.
///
/// Roland-derived, and the one deliberate exception to this project's no-redistribution rule: the
/// coefficients are computed by the engine at start-up rather than stored in the DLL, so obtaining
/// them means executing a 64-bit Windows binary. Shipping the file is what lets the engine have its
/// effects at all on every other host.
[[nodiscard]] std::string_view presets_json() noexcept;

} // namespace ts::assets
