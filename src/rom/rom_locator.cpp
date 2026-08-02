#include "tabulasonora/rom_locator.hpp"

#include <cstdlib>

namespace ts {
namespace {

namespace fs = std::filesystem;

/// The name the pinned build ships under, used for the working-directory fallback.
constexpr const char* rom_file_name = "SCCore.dll";

} // namespace

RomLocation locate_rom(const std::string& explicit_path)
{
    RomLocation location;

    if (!explicit_path.empty()) {
        location.path = explicit_path;
        return location;
    }

    if (const char* pinned = std::getenv(rom_path_variable); pinned != nullptr && *pinned != '\0') {
        location.searched.emplace_back(std::string{rom_path_variable} + '=' + pinned);
        std::error_code error;
        if (fs::exists(pinned, error)) {
            location.path = pinned;
            return location;
        }
    } else {
        location.searched.emplace_back(std::string{rom_path_variable} + " (not set)");
    }

    std::error_code error;
    const fs::path beside = fs::current_path(error) / rom_file_name;
    location.searched.push_back(beside.string());
    if (!error && fs::exists(beside, error)) {
        location.path = beside;
    }

    return location;
}

std::string rom_not_found_message(const RomLocation& location)
{
    std::string message = "no SCCore.dll found. Pin one with `export ";
    message += rom_path_variable;
    message += "=/path/to/SCCore.dll`, or pass --dll. Looked at:";

    for (const std::string& where : location.searched) {
        message += "\n  ";
        message += where;
    }

    return message;
}

} // namespace ts
