#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

class AddressSpace;

struct PrimeFieldProfile {
    std::uint32_t modulus_offset;

    // Recognize the compact ARM32 context by its standard reduction callback.
    // Contexts with options or specialized reductions keep their guest path.
    [[nodiscard]] static std::optional<PrimeFieldProfile> resolve(
        const AddressSpace& memory, std::uint32_t context,
        std::uint32_t standard_reduction);
};

} // namespace ilemu
