#pragma once

#include "tabulasonora/build_registry.hpp"
#include "tabulasonora/table_manifest.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ts {

/// How strictly `RomImage::open` checks that the file is the pinned build.
enum class RomVerification {
    /// Identify the build by size, PE timestamp and the full SHA-256. The default.
    ///
    /// Any build in the registry is accepted, not only the pinned one — a build the engine knows how
    /// to translate is as safe to read as the build the offsets were recorded against.
    full,
    /// Identify by size and PE timestamp but skip the SHA-256. Faster; use only in tight loops.
    ///
    /// Two builds sharing both would make this ambiguous; the registry rejects that rather than
    /// guess, so an ambiguous pair falls through to the same error as an unknown file.
    quick,
    /// Perform no checks at all, and assume the pinned build. Offsets will be wrong for any other.
    none,
};

/// Thrown when the supplied `SCCore.dll` is not a build this engine knows how to read.
class RomIdentityError : public std::runtime_error {
public:
    explicit RomIdentityError(const std::string& message) : std::runtime_error(message) {}
};

/// Thrown when a read falls in a range the identified build's map cannot place.
///
/// Distinct from `RomIdentityError` because it means something different: the file *is* a recognised
/// build, but this particular table could not be traced through its re-packed `.rdata`. Returning
/// zeroes for the hole would be silently wrong, so it is an error rather than a partial read.
class RomCoverageError : public std::runtime_error {
public:
    explicit RomCoverageError(const std::string& message) : std::runtime_error(message) {}
};

/// Read-only access to `SCCore.dll` *as a data file*.
///
/// The DLL is never loaded as code — no `LoadLibrary`, no `dlopen`, no native dependency. It is
/// opened as a plain file and sliced at the offsets recorded in `TableManifest`, which is what lets
/// this engine stay portable and run on hosts the original plugin never supported.
///
/// A file image reads positionally (`pread`) rather than through a memory map, so no 27 MB copy is
/// ever resident. A host without a filesystem supplies the bytes instead; see `from_memory`.
class RomImage {
public:
    /// Opens and verifies an `SCCore.dll`.
    ///
    /// Throws `RomIdentityError` if the file is not the pinned build, or `std::runtime_error` if it
    /// cannot be opened. The manifest defaults to the embedded one.
    [[nodiscard]] static RomImage open(const std::string& path,
                                       RomVerification verification = RomVerification::full,
                                       const TableManifest* manifest = nullptr);

    /// Wraps an `SCCore.dll` already held in memory, and verifies it.
    ///
    /// Nothing is copied — the memory is read in place — so the caller must keep it alive and
    /// unchanged for as long as the image is used, exactly as a file image needs its handle open.
    [[nodiscard]] static RomImage from_memory(std::span<const std::uint8_t> image,
                                              RomVerification verification = RomVerification::full,
                                              const TableManifest* manifest = nullptr,
                                              std::string name = "<memory>");

    RomImage(RomImage&&) noexcept;
    RomImage& operator=(RomImage&&) noexcept;
    RomImage(const RomImage&) = delete;
    RomImage& operator=(const RomImage&) = delete;
    ~RomImage();

    /// Path, or descriptive name, the image was opened from.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    /// Length of the file in bytes.
    [[nodiscard]] std::int64_t length() const noexcept { return length_; }

    /// The offset map these reads are interpreted through.
    [[nodiscard]] const TableManifest& manifest() const noexcept { return *manifest_; }

    /// The build this file was identified as, which every offset is translated through.
    [[nodiscard]] const BuildProfile& build() const noexcept { return *build_; }

    /// Reads into a caller-supplied buffer, filling it completely.
    ///
    /// `file_offset` is a **pinned-build** offset — the coordinate system `manifest.json` records —
    /// and is translated into this build's, gathering the pieces when the packing differs. On the
    /// pinned build that translation is the identity and this costs nothing.
    ///
    /// Throws `RomCoverageError` if any of the range is unmapped in this build, or
    /// `std::runtime_error` if the file ends before the buffer is filled.
    void read(std::int64_t file_offset, std::span<std::uint8_t> destination) const;

    /// Reads `length` bytes starting at a pinned-build `file_offset`.
    [[nodiscard]] std::vector<std::uint8_t> read(std::int64_t file_offset,
                                                 std::size_t length) const;

    /// Reads at an absolute offset in *this* file, with no translation.
    ///
    /// For data located per build rather than through the segment map — the wave ROM, whose banks
    /// are found by scanning block magic (see `wave_rom_base`).
    void read_raw(std::int64_t file_offset, std::span<std::uint8_t> destination) const;

    /// Reads `length` bytes at an absolute, untranslated offset.
    [[nodiscard]] std::vector<std::uint8_t> read_raw(std::int64_t file_offset,
                                                     std::size_t length) const;

    /// Absolute offset of a wave-ROM bank in this build, by manifest region name.
    ///
    /// Throws `std::out_of_range` if the build does not record that region.
    [[nodiscard]] std::int64_t wave_rom_base(std::string_view region) const;

    /// Reads the bytes of one cached table, exactly `entry.size` long.
    [[nodiscard]] std::vector<std::uint8_t> read(const TableEntry& entry) const;

    /// Reads the COFF `TimeDateStamp` from the PE header, or `0` if the file is not a PE image.
    [[nodiscard]] std::uint32_t read_pe_timestamp() const;

    /// Computes the SHA-256 of the whole file as lower-case hex.
    [[nodiscard]] std::string compute_sha256() const;

private:
    /// Where an image's bytes come from.
    ///
    /// The only thing the rest of the class needs of a source is a length and a positional read,
    /// which is exactly the shape both a file handle and a byte buffer offer.
    class Source {
    public:
        virtual ~Source() = default;
        [[nodiscard]] virtual std::int64_t length() const noexcept = 0;
        /// Reads at an absolute offset, returning how much was actually read; zero at the end.
        [[nodiscard]] virtual std::size_t read(std::span<std::uint8_t> destination,
                                               std::int64_t offset) const = 0;
    };

    class FileSource;
    class MemorySource;

    RomImage(std::string path, std::unique_ptr<Source> source, const TableManifest& manifest);

    /// Identifies the file against the build registry, setting `build_`.
    void identify(RomVerification verification);

    std::string path_;
    std::unique_ptr<Source> source_;
    const TableManifest* manifest_;
    const BuildProfile* build_ = nullptr;
    std::int64_t length_ = 0;
};

} // namespace ts
