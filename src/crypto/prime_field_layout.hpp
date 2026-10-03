// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Recognize the guest ARM32 prime-field context used by modular
// arithmetic.

#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

class AddressSpace;

struct PrimeFieldLayout {
    std::uint32_t modulus_offset;

    // Recognize compact and options-bearing ARM32 headers by their standard
    // reduction callback. Nonzero options and specialized reductions retain
    // firmware execution.
    [[nodiscard]] static std::optional<PrimeFieldLayout> resolve(
        const AddressSpace& memory, std::uint32_t context,
        std::uint32_t standard_reduction);
};

} // namespace ilemu
