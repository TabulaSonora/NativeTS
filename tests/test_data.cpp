#include "test_data.hpp"

#include "rom/sha256.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdlib>
#include <string>
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

} // namespace ts::testdata
