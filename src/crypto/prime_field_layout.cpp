// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Recognize the guest ARM32 prime-field context used by modular
// arithmetic.

#include "prime_field_layout.hpp"

#include "foundation/address_space.hpp"
#include <limits>

namespace ilemu {

std::optional<PrimeFieldLayout> PrimeFieldLayout::resolve(
    const AddressSpace& memory, std::uint32_t context,
    std::uint32_t standard_reduction)
{
    if (context > std::numeric_limits<std::uint32_t>::max() - 8U ||
        standard_reduction == 0U) {
        return std::nullopt;
    }
    constexpr PrimeFieldLayout compact_arm32 { 8U };
    const auto reduction = memory.read32(context + sizeof(std::uint32_t));
    if (reduction && (*reduction & ~1U) == (standard_reduction & ~1U))
        return compact_arm32;
    // The extended header inserts an options word before the callback.
    // Only ordinary integer residues can use the existing host arithmetic;
    // Montgomery or otherwise specialized contexts retain firmware execution.
    constexpr PrimeFieldLayout options_arm32 { 12U };
    if (context <= std::numeric_limits<std::uint32_t>::max() -
                       options_arm32.modulus_offset &&
        reduction && *reduction == 0U) {
        const auto extended_reduction = memory.read32(context + 8U);
        if (extended_reduction &&
            (*extended_reduction & ~1U) == (standard_reduction & ~1U)) {
            return options_arm32;
        }
    }
    return std::nullopt;
}

} // namespace ilemu
