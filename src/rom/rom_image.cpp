#include "tabulasonora/rom_image.hpp"

#include "dsp/fixed.hpp"
#include "rom/sha256.hpp"

#include "tabulasonora/build_registry.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <sstream>
#include <utility>

#ifdef _WIN32
#    include <io.h>
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace ts {
namespace {

/// Formats a PE `TimeDateStamp` the way a person reads it, so a mismatch message is actionable.
[[nodiscard]] std::string format_timestamp(std::uint32_t timestamp)
{
    const std::time_t seconds = static_cast<std::time_t>(timestamp);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif

    std::array<char, 32> text{};
    std::strftime(text.data(), text.size(), "%Y-%m-%d %H:%M:%S UTC", &utc);
    return std::string{text.data()};
}

/// Groups a byte count with thousands separators, matching the upstream diagnostics.
[[nodiscard]] std::string with_separators(std::int64_t value)
{
    std::string digits = std::to_string(value);
    for (std::size_t i = digits.size(); i > 3;) {
        i -= 3;
        digits.insert(i, ",");
    }
    return digits;
}

/// The message for a file that matches no build the engine knows.
///
/// It lists every build the registry does know, because the useful next step is almost always "you
/// have a different SOUND Canvas VA release than you thought" and the sizes make that obvious at a
/// glance. `SCCore.dll` carries no version resource, so there is nothing friendlier to report.
[[nodiscard]] std::string unknown_build_message(const std::string& path,
                                                std::int64_t length,
                                                std::uint32_t pe_timestamp,
                                                const std::string& sha256)
{
    std::ostringstream message;
    message << "'" << path << "' is not an SCCore.dll build this engine knows: "
            << with_separators(length) << " bytes, PE timestamp " << pe_timestamp << " ("
            << format_timestamp(pe_timestamp) << ')';
    if (!sha256.empty()) {
        message << ", SHA-256 " << sha256;
    }
    message << ". Known builds:";
    for (const BuildProfile& profile : BuildRegistry::defaults().builds()) {
        message << "\n  " << profile.id() << "  " << with_separators(profile.identity().size)
                << " bytes  " << format_timestamp(profile.identity().pe_timestamp) << "  "
                << profile.architecture() << "  " << profile.file_name();
    }
    return message.str();
}


} // namespace

// ---------------------------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------------------------

class RomImage::FileSource final : public RomImage::Source {
public:
    explicit FileSource(const std::string& path)
    {
#ifdef _WIN32
        handle_ = ::CreateFileA(path.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot open '" + path + "'.");
        }
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(handle_, &size) == 0) {
            ::CloseHandle(handle_);
            throw std::runtime_error("Cannot size '" + path + "'.");
        }
        length_ = static_cast<std::int64_t>(size.QuadPart);
#else
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) {
            throw std::runtime_error("Cannot open '" + path + "': " + std::strerror(errno));
        }
        struct stat info{};
        if (::fstat(fd_, &info) != 0) {
            ::close(fd_);
            throw std::runtime_error("Cannot size '" + path + "': " + std::strerror(errno));
        }
        length_ = static_cast<std::int64_t>(info.st_size);
#endif
    }

    ~FileSource() override
    {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            ::close(fd_);
        }
#endif
    }

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    [[nodiscard]] std::int64_t length() const noexcept override { return length_; }

    [[nodiscard]] std::size_t read(std::span<std::uint8_t> destination,
                                   std::int64_t offset) const override
    {
        if (destination.empty()) {
            return 0;
        }
#ifdef _WIN32
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFF);
        overlapped.OffsetHigh = static_cast<DWORD>(static_cast<std::uint64_t>(offset) >> 32);
        DWORD moved = 0;
        if (::ReadFile(handle_,
                       destination.data(),
                       static_cast<DWORD>(destination.size()),
                       &moved,
                       &overlapped)
            == 0) {
            if (::GetLastError() == ERROR_HANDLE_EOF) {
                return 0;
            }
            throw std::runtime_error("Read failed.");
        }
        return static_cast<std::size_t>(moved);
#else
        // Positional read: no seek state, so the image is safe to share between threads and a
        // concurrent read cannot disturb another's position.
        const ::ssize_t moved =
            ::pread(fd_, destination.data(), destination.size(), static_cast<::off_t>(offset));
        if (moved < 0) {
            throw std::runtime_error(std::string{"Read failed: "} + std::strerror(errno));
        }
        return static_cast<std::size_t>(moved);
#endif
    }

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    std::int64_t length_ = 0;
};

class RomImage::MemorySource final : public RomImage::Source {
public:
    explicit MemorySource(std::span<const std::uint8_t> image) : image_(image) {}

    [[nodiscard]] std::int64_t length() const noexcept override
    {
        return static_cast<std::int64_t>(image_.size());
    }

    [[nodiscard]] std::size_t read(std::span<std::uint8_t> destination,
                                   std::int64_t offset) const override
    {
        if (offset < 0 || static_cast<std::size_t>(offset) >= image_.size()) {
            return 0;
        }
        const std::size_t available =
            std::min(destination.size(), image_.size() - static_cast<std::size_t>(offset));
        std::memcpy(destination.data(), image_.data() + offset, available);
        return available;
    }

private:
    std::span<const std::uint8_t> image_;
};

// ---------------------------------------------------------------------------------------------
// RomImage
// ---------------------------------------------------------------------------------------------

RomImage::RomImage(std::string path, std::unique_ptr<Source> source, const TableManifest& manifest)
    : path_(std::move(path)),
      source_(std::move(source)),
      manifest_(&manifest),
      length_(source_->length())
{
}

RomImage::RomImage(RomImage&&) noexcept = default;
RomImage& RomImage::operator=(RomImage&&) noexcept = default;
RomImage::~RomImage() = default;

RomImage
RomImage::open(const std::string& path, RomVerification verification, const TableManifest* manifest)
{
    if (path.empty()) {
        throw std::invalid_argument("A ROM path is required.");
    }

    const TableManifest& map = manifest != nullptr ? *manifest : TableManifest::defaults();
    RomImage image{path, std::make_unique<FileSource>(path), map};

    image.identify(verification);
    return image;
}

RomImage RomImage::from_memory(std::span<const std::uint8_t> bytes,
                               RomVerification verification,
                               const TableManifest* manifest,
                               std::string name)
{
    const TableManifest& map = manifest != nullptr ? *manifest : TableManifest::defaults();
    RomImage image{std::move(name), std::make_unique<MemorySource>(bytes), map};

    image.identify(verification);
    return image;
}

void RomImage::read_raw(std::int64_t file_offset, std::span<std::uint8_t> destination) const
{
    if (file_offset < 0) {
        throw std::out_of_range("A file offset cannot be negative.");
    }

    std::size_t total = 0;
    while (total < destination.size()) {
        const std::size_t moved = source_->read(destination.subspan(total),
                                                file_offset + static_cast<std::int64_t>(total));
        if (moved == 0) {
            std::ostringstream message;
            message << "Short read at offset 0x" << std::hex << file_offset << std::dec
                    << ": wanted " << destination.size() << " bytes, got " << total << '.';
            throw std::runtime_error(message.str());
        }
        total += moved;
    }
}

std::vector<std::uint8_t> RomImage::read_raw(std::int64_t file_offset, std::size_t length) const
{
    std::vector<std::uint8_t> buffer(length);
    read_raw(file_offset, std::span<std::uint8_t>{buffer});
    return buffer;
}

void RomImage::read(std::int64_t file_offset, std::span<std::uint8_t> destination) const
{
    if (file_offset < 0) {
        throw std::out_of_range("A file offset cannot be negative.");
    }
    if (destination.empty()) {
        return;
    }

    // On the pinned build this is one piece at the same offset, so the gather costs a single read.
    const auto pieces = build_->map_range(file_offset, static_cast<std::int64_t>(destination.size()));
    std::size_t written = 0;
    for (const MappedPiece& piece : pieces) {
        const auto length = static_cast<std::size_t>(piece.length);
        if (!piece.mapped()) {
            std::ostringstream message;
            message << "'" << path_ << "' is " << build_->id() << ", whose .rdata is packed "
                    << "differently from the pinned build; " << length << " byte"
                    << (length == 1 ? "" : "s") << " at pinned offset 0x" << std::hex
                    << piece.pinned_offset << std::dec
                    << " could not be traced into it. That data is present in the file but its "
                       "location is not proven, so reading it would be a guess.";
            throw RomCoverageError(message.str());
        }
        read_raw(*piece.target_offset, destination.subspan(written, length));
        written += length;
    }
}

std::vector<std::uint8_t> RomImage::read(std::int64_t file_offset, std::size_t length) const
{
    std::vector<std::uint8_t> buffer(length);
    read(file_offset, std::span<std::uint8_t>{buffer});
    return buffer;
}

std::vector<std::uint8_t> RomImage::read(const TableEntry& entry) const
{
    // A whole-table offset proven by content search beats reassembling the same bytes out of
    // segments, so prefer it where the build records one.
    if (const auto direct = build_->table_offset(entry.name)) {
        return read_raw(*direct, static_cast<std::size_t>(entry.size));
    }
    return read(entry.file_offset, static_cast<std::size_t>(entry.size));
}

std::int64_t RomImage::wave_rom_base(std::string_view region) const
{
    if (const auto offset = build_->wave_rom_offset(region)) {
        return *offset;
    }
    throw std::out_of_range("Build '" + build_->id() + "' does not record wave-ROM region '"
                            + std::string{region} + "'.");
}

std::uint32_t RomImage::read_pe_timestamp() const
{
    std::array<std::uint8_t, 4> four{};
    read_raw(0x3C, four);

    const std::int64_t pe_header = static_cast<std::int64_t>(fx::read_u32le(four.data()));
    if (pe_header <= 0 || pe_header + 8 > length_) {
        return 0;
    }

    std::array<std::uint8_t, 4> signature{};
    read_raw(pe_header, signature);
    if (signature[0] != 'P' || signature[1] != 'E' || signature[2] != 0 || signature[3] != 0) {
        return 0;
    }

    read_raw(pe_header + 8, four);
    return fx::read_u32le(four.data());
}

std::string RomImage::compute_sha256() const
{
    Sha256 hash;
    std::vector<std::uint8_t> buffer(1u << 20);

    for (std::int64_t offset = 0; offset < length_;) {
        const auto wanted = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(buffer.size()), length_ - offset));
        const std::size_t moved = source_->read(std::span{buffer}.first(wanted), offset);
        if (moved == 0) {
            break;
        }
        hash.update(buffer.data(), moved);
        offset += static_cast<std::int64_t>(moved);
    }

    return hash.finish_hex();
}

void RomImage::identify(RomVerification verification)
{
    const BuildRegistry& registry = BuildRegistry::defaults();

    // `none` skips identification entirely and assumes the pinned build, which is what the caller
    // asked for: it is the mode for poking at an unrecognised file, where wrong offsets are the
    // point rather than an accident.
    if (verification == RomVerification::none) {
        build_ = &registry.pinned();
        return;
    }

    // Size first. A file matching no build's size cannot be any of them, and settling that before
    // touching the PE header keeps a stray short file reporting "not a build I know" rather than
    // failing inside the header reader, which is both the wrong error and the wrong exception type.
    const bool plausible_size = std::any_of(registry.builds().begin(), registry.builds().end(),
                                            [this](const BuildProfile& profile) {
                                                return profile.identity().size == length_;
                                            });
    if (!plausible_size) {
        throw RomIdentityError(unknown_build_message(path_, length_, 0, {}));
    }

    const std::uint32_t timestamp = read_pe_timestamp();
    const BuildProfile* candidate = registry.find_by_size_and_timestamp(length_, timestamp);

    if (verification == RomVerification::quick) {
        if (candidate == nullptr) {
            throw RomIdentityError(unknown_build_message(path_, length_, timestamp, {}));
        }
        build_ = candidate;
        return;
    }

    const std::string sha256 = compute_sha256();
    const BuildProfile* found = registry.find_by_sha256(sha256);
    if (found == nullptr) {
        throw RomIdentityError(unknown_build_message(path_, length_, timestamp, sha256));
    }
    build_ = found;
}

} // namespace ts
