#include "tabulasonora/build_registry.hpp"

#include "rom/embedded_assets.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>

namespace ts {
namespace {

using nlohmann::json;

[[nodiscard]] const json& require(const json& object, const char* property)
{
    const auto found = object.find(property);
    if (found == object.end()) {
        throw std::runtime_error(std::string{"builds.json is missing '"} + property + "'.");
    }
    return *found;
}

[[nodiscard]] std::string string_or_empty(const json& object, const char* property)
{
    const auto found = object.find(property);
    if (found == object.end() || !found->is_string()) {
        return {};
    }
    return found->get<std::string>();
}

/// Lower-cases a digest so hash comparisons fold by construction rather than at every call site.
[[nodiscard]] std::string lower_hex(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c);
    });
    return text;
}

[[nodiscard]] std::int64_t parse_hex(const std::string& text)
{
    if (text.empty()) {
        throw std::runtime_error("builds.json has an empty hex value.");
    }
    return static_cast<std::int64_t>(std::stoll(text, nullptr, 16));
}

} // namespace

std::vector<MappedPiece> BuildProfile::map_range(std::int64_t pinned_offset,
                                                 std::int64_t length) const
{
    std::vector<MappedPiece> pieces;
    if (length <= 0) {
        return pieces;
    }

    // The pinned build is its own translation, so it needs no segments and never reports a hole.
    if (pinned_) {
        pieces.push_back({pinned_offset, pinned_offset, length});
        return pieces;
    }

    const std::int64_t end = pinned_offset + length;
    std::int64_t position = pinned_offset;

    // Segments are sorted and non-overlapping, so a linear sweep from the first that can intersect
    // is enough; binary-searching the start keeps a 16 KB read off an 800-segment scan.
    auto first = std::lower_bound(segments_.begin(), segments_.end(), pinned_offset,
                                  [](const BuildSegment& segment, std::int64_t value) {
                                      return segment.pinned_end <= value;
                                  });

    for (auto it = first; it != segments_.end() && it->pinned_start < end; ++it) {
        const std::int64_t low = std::max(it->pinned_start, position);
        const std::int64_t high = std::min(it->pinned_end, end);
        if (high <= low) {
            continue;
        }
        if (low > position) {
            pieces.push_back({position, std::nullopt, low - position});
        }
        pieces.push_back({low, low - it->shift, high - low});
        position = high;
    }

    if (position < end) {
        pieces.push_back({position, std::nullopt, end - position});
    }
    return pieces;
}

bool BuildProfile::covers(std::int64_t pinned_offset, std::int64_t length) const
{
    for (const MappedPiece& piece : map_range(pinned_offset, length)) {
        if (!piece.mapped()) {
            return false;
        }
    }
    return true;
}

std::optional<std::int64_t> BuildProfile::table_offset(std::string_view name) const
{
    for (const auto& [table, offset] : tables_) {
        if (table == name) {
            return offset;
        }
    }
    return std::nullopt;
}

bool BuildProfile::covers_table(const TableEntry& entry) const
{
    return table_offset(entry.name).has_value() || covers(entry.file_offset, entry.size);
}

std::optional<std::int64_t> BuildProfile::wave_rom_offset(std::string_view region) const
{
    for (const auto& [name, offset] : wave_rom_) {
        if (name == region) {
            return offset;
        }
    }
    return std::nullopt;
}

std::vector<TableCoverage> BuildProfile::coverage(const TableManifest& manifest) const
{
    std::vector<TableCoverage> out;
    out.reserve(manifest.cached_tables().size());
    for (const TableEntry& entry : manifest.cached_tables()) {
        TableCoverage row;
        row.name = entry.name;
        row.size = entry.size;
        if (table_offset(entry.name)) {
            row.mapped_bytes = entry.size;
        } else {
            for (const MappedPiece& piece : map_range(entry.file_offset, entry.size)) {
                if (piece.mapped()) {
                    row.mapped_bytes += piece.length;
                }
            }
        }
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<TableCoverage> BuildProfile::incomplete_tables(const TableManifest& manifest) const
{
    std::vector<TableCoverage> out;
    for (TableCoverage& row : coverage(manifest)) {
        if (!row.complete()) {
            out.push_back(std::move(row));
        }
    }
    return out;
}

BuildRegistry BuildRegistry::parse(std::string_view text)
{
    const json document = json::parse(text, nullptr, true, true);
    BuildRegistry registry;

    for (const json& item : require(document, "builds")) {
        BuildProfile profile;
        profile.id_ = string_or_empty(item, "id");
        profile.file_name_ = string_or_empty(item, "file_name");
        profile.description_ = string_or_empty(item, "description");
        profile.architecture_ = string_or_empty(item, "architecture");
        profile.pinned_ = item.value("pinned", false);

        DllIdentity identity;
        identity.file_name = profile.file_name_;
        identity.product = string_or_empty(item, "product");
        identity.version = string_or_empty(item, "version");
        identity.size = require(item, "size").get<std::int64_t>();
        identity.sha256 = lower_hex(string_or_empty(item, "sha256"));
        identity.sha1 = lower_hex(string_or_empty(item, "sha1"));
        identity.md5 = lower_hex(string_or_empty(item, "md5"));
        identity.pe_timestamp = require(item, "pe_timestamp").get<std::uint32_t>();
        profile.identity_ = std::move(identity);

        if (const auto tables = item.find("tables"); tables != item.end()) {
            for (const auto& [name, offset] : tables->items()) {
                profile.tables_.emplace_back(name, parse_hex(offset.get<std::string>()));
            }
        }

        if (const auto rom = item.find("wave_rom"); rom != item.end()) {
            for (const auto& [name, offset] : rom->items()) {
                profile.wave_rom_.emplace_back(name, parse_hex(offset.get<std::string>()));
            }
        }

        if (const auto segments = item.find("segments"); segments != item.end()) {
            profile.segments_.reserve(segments->size());
            for (const json& triple : *segments) {
                if (triple.size() != 3) {
                    throw std::runtime_error("builds.json: a segment is not a [start, end, shift] triple.");
                }
                BuildSegment segment;
                segment.pinned_start = triple[0].get<std::int64_t>();
                segment.pinned_end = triple[1].get<std::int64_t>();
                segment.shift = triple[2].get<std::int64_t>();
                if (segment.pinned_end <= segment.pinned_start) {
                    throw std::runtime_error("builds.json: a segment is empty or inverted.");
                }
                profile.segments_.push_back(segment);
            }
            std::sort(profile.segments_.begin(), profile.segments_.end(),
                      [](const BuildSegment& a, const BuildSegment& b) {
                          return a.pinned_start < b.pinned_start;
                      });
            for (std::size_t i = 1; i < profile.segments_.size(); ++i) {
                if (profile.segments_[i].pinned_start < profile.segments_[i - 1].pinned_end) {
                    throw std::runtime_error("builds.json: segments overlap; the map is ambiguous.");
                }
            }
        }

        registry.builds_.push_back(std::move(profile));
    }

    if (registry.builds_.empty()) {
        throw std::runtime_error("builds.json lists no builds.");
    }
    return registry;
}

const BuildRegistry& BuildRegistry::defaults()
{
    static const BuildRegistry instance = parse(assets::builds_json());
    return instance;
}

const BuildProfile* BuildRegistry::find_by_sha256(std::string_view sha256) const
{
    const std::string wanted = lower_hex(std::string{sha256});
    for (const BuildProfile& profile : builds_) {
        if (profile.identity().sha256 == wanted) {
            return &profile;
        }
    }
    return nullptr;
}

const BuildProfile* BuildRegistry::find_by_size_and_timestamp(std::int64_t size,
                                                              std::uint32_t pe_timestamp) const
{
    const BuildProfile* found = nullptr;
    for (const BuildProfile& profile : builds_) {
        if (profile.identity().size == size && profile.identity().pe_timestamp == pe_timestamp) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &profile;
        }
    }
    return found;
}

const BuildProfile& BuildRegistry::pinned() const
{
    for (const BuildProfile& profile : builds_) {
        if (profile.pinned()) {
            return profile;
        }
    }
    throw std::runtime_error("builds.json marks no build as pinned.");
}

} // namespace ts
