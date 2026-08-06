#include "tabulasonora/tone.hpp"

#include <algorithm>

namespace ts {
namespace {

/// Reads a fixed-width ASCII name field, dropping trailing padding.
///
/// The ROM stores these as Latin-1, but every name in the table is plain ASCII; a byte above 127
/// would come through here as-is rather than as a code point, which is the one way this differs
/// from the managed original. Both space and NUL pad, and unused records pad with one or the other.
[[nodiscard]] std::string read_name(std::span<const std::uint8_t> field)
{
    std::string name;
    name.reserve(field.size());
    for (std::uint8_t byte : field) {
        name.push_back(static_cast<char>(byte));
    }

    const auto last = name.find_last_not_of(std::string_view{" \0", 2});
    name.erase(last == std::string::npos ? 0 : last + 1);
    return name;
}

} // namespace

Tone Tone::read(std::span<const std::uint8_t> tone_table, int number)
{
    const auto offset = static_cast<std::size_t>(number) * stride;

    Tone tone;
    tone.number_ = number;
    tone.name_ = read_name(tone_table.subspan(offset, name_length));
    tone.level_ = tone_table[offset + 0x0C];

    tone.partials_.reserve(partial_slots);
    for (int slot = 0; slot < partial_slots; ++slot) {
        const std::size_t block =
            offset + header_size + (static_cast<std::size_t>(slot) * PartialParameters::stride);
        const PartialParameters partial{tone_table.data() + block, tone_table.data() + offset};
        if (partial.is_present()) {
            tone.partials_.push_back(partial);
        }
    }

    return tone;
}

} // namespace ts
