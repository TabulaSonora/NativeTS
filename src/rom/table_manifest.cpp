#include "tabulasonora/table_manifest.hpp"

#include "rom/embedded_assets.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <stdexcept>
#include <string>

namespace ts {
namespace {

using nlohmann::json;

/// Reads a string property, or returns an empty string when it is absent or not a string.
[[nodiscard]] std::string string_or_empty(const json& object, const char* property)
{
    const auto found = object.find(property);
    if (found == object.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

/// Parses a `0x`-prefixed hex string. Returns nothing for an absent, empty or malformed value.
///
/// Every offset in the manifest is recorded as a hex *string* rather than a number, so this is the
/// only path by which an offset enters the engine.
[[nodiscard]] std::optional<std::int64_t> parse_hex(const std::string& text)
{
    std::string_view span{text};
    if (span.empty()) {
        return std::nullopt;
    }

    if (span.size() > 2 && span[0] == '0' && (span[1] == 'x' || span[1] == 'X')) {
        span.remove_prefix(2);
    }

    std::int64_t value = 0;
    const char* first = span.data();
    const char* last = first + span.size();
    const auto result = std::from_chars(first, last, value, 16);

    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return value;
}

/// Reads a hex-string property.
[[nodiscard]] std::optional<std::int64_t> hex_property(const json& object, const char* property)
{
    return parse_hex(string_or_empty(object, property));
}

/// Reads a required property, failing with a message that names it.
[[nodiscard]] const json& require(const json& object, const char* property)
{
    const auto found = object.find(property);
    if (found == object.end()) {
        throw std::runtime_error(std::string{"manifest.json is missing '"} + property + "'.");
    }
    return *found;
}

/// Lower-cases a hex digest so comparisons against a computed hash are case-insensitive by
/// construction rather than by remembering to fold at every site.
[[nodiscard]] std::string require_hex_digest(const json& object, const char* property)
{
    auto text = require(object, property).get<std::string>();
    for (char& c : text) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

} // namespace

TableManifest TableManifest::parse(std::string_view text)
{
    const auto root = json::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/false);

    TableManifest manifest;

    const json& dll = require(root, "dll");
    manifest.dll_ = DllIdentity{
        .file_name = string_or_empty(dll, "filename").empty() ? "SCCore.dll"
                                                              : string_or_empty(dll, "filename"),
        .product = string_or_empty(dll, "product"),
        .version = string_or_empty(dll, "version"),
        .size = require(dll, "size").get<std::int64_t>(),
        .sha256 = require_hex_digest(dll, "sha256"),
        .sha1 = require_hex_digest(dll, "sha1"),
        .md5 = require_hex_digest(dll, "md5"),
        .pe_timestamp = static_cast<std::uint32_t>(require(dll, "pe_timestamp").get<std::int64_t>()),
    };

    const auto image_base = hex_property(root, "image_base");
    if (!image_base) {
        throw std::runtime_error("manifest.json is missing 'image_base'.");
    }
    manifest.image_base_ = *image_base;

    for (const json& entry : require(root, "cached_tables")) {
        const auto file_offset = hex_property(entry, "file_offset");
        if (!file_offset) {
            // An entry with no file offset cannot be extracted; the generator emits these only
            // when it failed to locate the bytes in the DLL.
            continue;
        }

        auto match = string_or_empty(entry, "match");

        manifest.tables_.push_back(TableEntry{
            .name = require(entry, "name").get<std::string>(),
            .symbol = string_or_empty(entry, "symbol"),
            .subsystem = string_or_empty(entry, "subsystem"),
            .dtype = string_or_empty(entry, "dtype"),
            .shape = string_or_empty(entry, "shape"),
            .size = require(entry, "size").get<std::int32_t>(),
            .file_offset = *file_offset,
            .va = hex_property(entry, "va"),
            .section_adjust = hex_property(entry, "section_adjust"),
            .match = match.empty() ? "full" : std::move(match),
            .purpose = string_or_empty(entry, "purpose"),
        });
    }

    for (const json& entry : require(root, "live_regions")) {
        // Wave-ROM banks carry 'file_offset'; the drum tables carry 'va', but those values are
        // already file offsets (no image base applied). Either way it addresses the file.
        auto offset = hex_property(entry, "file_offset");
        if (!offset) {
            offset = hex_property(entry, "va");
        }
        if (!offset) {
            throw std::runtime_error("Live region '" + string_or_empty(entry, "name")
                                     + "' has neither file_offset nor va.");
        }

        std::optional<std::int64_t> size;
        if (const auto found = entry.find("size"); found != entry.end() && found->is_number()) {
            size = found->get<std::int64_t>();
        }

        manifest.regions_.push_back(LiveRegion{
            .name = require(entry, "name").get<std::string>(),
            .symbol = string_or_empty(entry, "symbol"),
            .subsystem = string_or_empty(entry, "subsystem"),
            .dtype = string_or_empty(entry, "dtype"),
            .file_offset = *offset,
            .size = size,
            .purpose = string_or_empty(entry, "purpose"),
        });
    }

    manifest.index();
    return manifest;
}

void TableManifest::index()
{
    by_name_.clear();
    regions_by_name_.clear();

    for (std::size_t i = 0; i < tables_.size(); ++i) {
        by_name_.emplace(tables_[i].name, i);
    }
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        regions_by_name_.emplace(regions_[i].name, i);
    }
}

const TableEntry& TableManifest::table(std::string_view name) const
{
    const auto found = by_name_.find(name);
    if (found == by_name_.end()) {
        throw std::out_of_range("No cached table named '" + std::string{name} + "' in the manifest.");
    }
    return tables_[found->second];
}

const LiveRegion& TableManifest::region(std::string_view name) const
{
    const auto found = regions_by_name_.find(name);
    if (found == regions_by_name_.end()) {
        throw std::out_of_range("No live region named '" + std::string{name} + "' in the manifest.");
    }
    return regions_[found->second];
}

const TableManifest& TableManifest::defaults()
{
    // Parsed on first use. Function-local statics are initialised thread-safely, which is what the
    // upstream build gets from a thread-safe Lazy<T>.
    static const TableManifest instance = parse(assets::manifest_json());
    return instance;
}

} // namespace ts
