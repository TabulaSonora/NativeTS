#pragma once

#include <string_view>

/// Data files compiled into the library.
///
/// Only the offset map, which is original work. Everything it points at — the wave ROM, the synth
/// tables, and the effect coefficients `EffectProgrammer` decodes — stays in the DLL you supply.
namespace ts::assets {

/// The table offset map — `manifest.json`, the pinned DLL identity plus every table's byte-exact
/// location. Embedded so the engine is self-describing with no data files on disk.
[[nodiscard]] std::string_view manifest_json() noexcept;

/// The build registry — `builds.json`, every `SCCore.dll` build the engine can read plus, for each
/// non-pinned one, the piecewise map translating pinned offsets into it.
[[nodiscard]] std::string_view builds_json() noexcept;

} // namespace ts::assets
