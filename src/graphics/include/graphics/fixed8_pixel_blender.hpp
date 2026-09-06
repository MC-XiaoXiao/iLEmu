#pragma once

#include <cstdint>
#include <span>

namespace ilemu {

// Premultiplied pixel arithmetic with a 256-step, truncating alpha factor.
// This is the integer scanline convention; normalized GLES blending differs.
class Fixed8PixelBlender {
public:
    static void source_over(std::span<const std::uint32_t> source,
        std::span<const std::uint32_t> destination,
        std::span<std::uint32_t> result);
};

} // namespace ilemu
