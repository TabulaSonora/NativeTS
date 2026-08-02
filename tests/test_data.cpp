#include "test_data.hpp"

#include <catch2/catch_test_macros.hpp>

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

[[nodiscard]] const fs::path& repository_root()
{
    static const fs::path root{TS_REPOSITORY_ROOT};
    return root;
}

} // namespace

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
