#include "tabulasonora/rom_image.hpp"

#include "dsp/fixed.hpp"
#include "rom/sha256.hpp"

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

    if (verification != RomVerification::none) {
        image.verify(verification);
    }
    return image;
}

RomImage RomImage::from_memory(std::span<const std::uint8_t> bytes,
                               RomVerification verification,
                               const TableManifest* manifest,
                               std::string name)
{
    const TableManifest& map = manifest != nullptr ? *manifest : TableManifest::defaults();
    RomImage image{std::move(name), std::make_unique<MemorySource>(bytes), map};

    if (verification != RomVerification::none) {
        image.verify(verification);
    }
    return image;
}

void RomImage::read(std::int64_t file_offset, std::span<std::uint8_t> destination) const
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

std::vector<std::uint8_t> RomImage::read(std::int64_t file_offset, std::size_t length) const
{
    std::vector<std::uint8_t> buffer(length);
    read(file_offset, std::span<std::uint8_t>{buffer});
    return buffer;
}

std::vector<std::uint8_t> RomImage::read(const TableEntry& entry) const
{
    return read(entry.file_offset, static_cast<std::size_t>(entry.size));
}

std::uint32_t RomImage::read_pe_timestamp() const
{
    std::array<std::uint8_t, 4> four{};
    read(0x3C, four);

    const std::int64_t pe_header = static_cast<std::int64_t>(fx::read_u32le(four.data()));
    if (pe_header <= 0 || pe_header + 8 > length_) {
        return 0;
    }

    std::array<std::uint8_t, 4> signature{};
    read(pe_header, signature);
    if (signature[0] != 'P' || signature[1] != 'E' || signature[2] != 0 || signature[3] != 0) {
        return 0;
    }

    read(pe_header + 8, four);
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

void RomImage::verify(RomVerification verification) const
{
    const DllIdentity& expected = manifest_->dll();

    if (length_ != expected.size) {
        throw RomIdentityError("'" + path_ + "' is " + with_separators(length_)
                               + " bytes; the pinned build — the one that ships with "
                               + expected.product + ' ' + expected.version + " — is "
                               + with_separators(expected.size)
                               + " bytes. A different build moves every table offset.");
    }

    const std::uint32_t timestamp = read_pe_timestamp();
    if (timestamp != expected.pe_timestamp) {
        throw RomIdentityError("'" + path_ + "' has PE timestamp " + std::to_string(timestamp)
                               + " (" + format_timestamp(timestamp) + "); the pinned build is "
                               + std::to_string(expected.pe_timestamp) + " ("
                               + format_timestamp(expected.pe_timestamp) + ").");
    }

    if (verification != RomVerification::full) {
        return;
    }

    const std::string sha256 = compute_sha256();
    if (sha256 != expected.sha256) {
        throw RomIdentityError("'" + path_ + "' has SHA-256 " + sha256 + "; the pinned build is "
                               + expected.sha256
                               + ". Table offsets are only valid for that exact build.");
    }
}

} // namespace ts
