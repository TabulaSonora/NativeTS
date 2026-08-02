#include "tabulasonora/partial_parameters.hpp"

#include <algorithm>

namespace ts {

int PartialParameters::transposed_key(int note) const noexcept
{
    const int key = note + (key_transpose() - 0x40) + (0x40 - key_center());
    return std::clamp(key, 0, 0x7F);
}

} // namespace ts
