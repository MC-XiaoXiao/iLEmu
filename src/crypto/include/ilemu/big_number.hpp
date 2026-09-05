#pragma once

#include <cstddef>
#include <span>

namespace ilemu {

// Unsigned, little-endian integers. The caller owns the fixed-width result.
class BigNumberArithmetic {
public:
    [[nodiscard]] static bool power_modulo(std::span<const std::byte> base,
        std::span<const std::byte> exponent, std::span<const std::byte> modulus,
        std::span<std::byte> result);
};

} // namespace ilemu
