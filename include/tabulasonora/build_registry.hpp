#pragma once

#include "tabulasonora/table_manifest.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ts {

/// One run of bytes that sits at the same place, modulo a fixed shift, in two builds.
///
/// `target_offset = pinned_offset - shift` for every offset in `[pinned_start, pinned_end)`.
struct BuildSegment {
    std::int64_t pinned_start = 0;
    std::int64_t pinned_end = 0;
    std::int64_t shift = 0;

    [[nodiscard]] std::int64_t length() const noexcept { return pinned_end - pinned_start; }
};

/// A contiguous piece of a requested range, resolved into one build's file offsets.
struct MappedPiece {
    /// Offset within the pinned build.
    std::int64_t pinned_offset = 0;
    /// Where those bytes live in this build, or nothing when the range is unmapped.
    std::optional<std::int64_t> target_offset;
    /// Length in bytes.
    std::int64_t length = 0;

    [[nodiscard]] bool mapped() const noexcept { return target_offset.has_value(); }
};

/// How completely one table can be read out of a given build.
struct TableCoverage {
    std::string name;
    std::int64_t size = 0;
    std::int64_t mapped_bytes = 0;

    [[nodiscard]] bool complete() const noexcept { return mapped_bytes >= size; }
};

/// One `SCCore.dll` build the engine knows how to read.
///
/// The manifest records its offsets in one build's coordinates — the 2019 build, because that is the
/// one the engine's behaviour was reverse-engineered from. Roland shipped earlier releases too, and
/// between them `.rdata` was re-packed: the table data is the same, but it was split and re-ordered,
/// so a table can span several differently-shifted segments (see `specv2/docs/DLL_VERSIONS.md`).
///
/// Fitting those older builds is about *acquiring the data*, not about tolerating a worse copy. The
/// engine ships no Roland data; it needs a DLL the reader already owns, and the more releases it can
/// read, the fewer readers have to go hunting for one specific file. So a non-pinned build is
/// described as a *translation* of the reference offsets rather than an offset map of its own, which
/// keeps `manifest.json` the single source of truth for what a table *is*.
class BuildProfile {
public:
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& file_name() const noexcept { return file_name_; }
    [[nodiscard]] const std::string& description() const noexcept { return description_; }
    /// `x64` or `x86`. Only matters for diagnostics; the DLL is read as data, never executed.
    [[nodiscard]] const std::string& architecture() const noexcept { return architecture_; }
    [[nodiscard]] const DllIdentity& identity() const noexcept { return identity_; }

    /// True for the build the manifest's offsets are expressed in, whose mapping is the identity.
    ///
    /// It is the *reference coordinate system*, not a better build: the engine's behaviour was
    /// reverse-engineered from this one, so it is where every offset was first recorded. The tables
    /// and wave ROM are the same data in every build here — rendering both through the real DLL
    /// agrees to within one 16-bit LSB — so a reader who owns an older release is not getting a
    /// lesser copy, only a differently packed one.
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    [[nodiscard]] const std::vector<BuildSegment>& segments() const noexcept { return segments_; }

    /// Whether this build can supply a whole table, by exact offset or through the segment map.
    [[nodiscard]] bool covers_table(const TableEntry& entry) const;

    /// Translates a pinned file range into this build's, splitting where the packing differs.
    ///
    /// The pieces tile the request in order. A piece with no `target_offset` is a range this build's
    /// map cannot place; callers must treat that as missing data rather than as zeroes.
    [[nodiscard]] std::vector<MappedPiece> map_range(std::int64_t pinned_offset,
                                                     std::int64_t length) const;

    /// Whether every byte of a pinned range resolves in this build.
    [[nodiscard]] bool covers(std::int64_t pinned_offset, std::int64_t length) const;

    /// A whole-table offset in this build, for a table found byte-exact there.
    ///
    /// Preferred over the segment map when present: it is a proven offset for the entire table
    /// rather than a reassembly, and it reaches tables too short for any window search to anchor.
    [[nodiscard]] std::optional<std::int64_t> table_offset(std::string_view name) const;

    /// The wave-ROM bank offset in this build, by manifest region name.
    ///
    /// The ROM is excluded from the segment map — 24 MB of bytes identical across every build would
    /// swamp it — and the two banks are stored in a different order per build, so their offsets are
    /// recorded per build instead. Returns nothing for a region this build does not name.
    [[nodiscard]] std::optional<std::int64_t> wave_rom_offset(std::string_view region) const;

    /// Per-table coverage of this build, for reporting what a build can and cannot supply.
    [[nodiscard]] std::vector<TableCoverage> coverage(const TableManifest& manifest) const;

    /// The tables this build cannot supply in full, in manifest order.
    [[nodiscard]] std::vector<TableCoverage> incomplete_tables(const TableManifest& manifest) const;

private:
    friend class BuildRegistry;

    std::string id_;
    std::string file_name_;
    std::string description_;
    std::string architecture_;
    DllIdentity identity_;
    bool pinned_ = false;
    std::vector<BuildSegment> segments_;
    std::vector<std::pair<std::string, std::int64_t>> wave_rom_;
    std::vector<std::pair<std::string, std::int64_t>> tables_;
};

/// Every build the engine recognises, keyed by content hash.
///
/// A build is identified by its SHA-256, because `SCCore.dll` carries no version resource at all —
/// not even a `FileVersion` — so there is nothing else to go on.
class BuildRegistry {
public:
    /// The registry embedded in this library. Parsed once, on first use.
    [[nodiscard]] static const BuildRegistry& defaults();

    /// Parses a registry from a JSON document.
    [[nodiscard]] static BuildRegistry parse(std::string_view json);

    [[nodiscard]] const std::vector<BuildProfile>& builds() const noexcept { return builds_; }

    /// Finds a build by lower-case hex SHA-256, or nothing if it is not one we know.
    [[nodiscard]] const BuildProfile* find_by_sha256(std::string_view sha256) const;

    /// Finds a build by size and PE timestamp — the cheap check, before hashing 27 MB.
    ///
    /// Returns nothing when no build matches, and also when more than one does, since an ambiguous
    /// answer is worse than none: the caller should fall back to the hash rather than guess.
    [[nodiscard]] const BuildProfile* find_by_size_and_timestamp(std::int64_t size,
                                                                 std::uint32_t pe_timestamp) const;

    /// The build every manifest offset is pinned to.
    [[nodiscard]] const BuildProfile& pinned() const;

private:
    BuildRegistry() = default;

    std::vector<BuildProfile> builds_;
};

} // namespace ts
