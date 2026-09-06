#pragma once

#include <cstdint>
#include <span>

namespace ilemu {

// Scanline sampling with signed 16.16 coordinates and truncating eight-bit
// interpolation weights. This preserves the integer software-renderer ABI.
class FixedPointTextureSampler {
public:
    static void sample(std::span<const std::uint32_t> first_row,
        std::span<const std::uint32_t> second_row, std::uint32_t coordinate,
        std::uint32_t increment, std::int32_t maximum_coordinate,
        std::uint32_t vertical_weight, bool linear, bool opaque,
        std::span<std::uint32_t> destination);
};

} // namespace ilemu
