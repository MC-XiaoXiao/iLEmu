#include "graphics/fixed8_pixel_blender.hpp"

namespace ilemu {

void Fixed8PixelBlender::source_over(std::span<const std::uint32_t> source,
    std::span<const std::uint32_t> destination,
    std::span<std::uint32_t> result)
{
    constexpr auto lanes = 0x00ff00ffU;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto factor = 256U - (source[index] >> 24U);
        const auto pixel = destination[index];
        const auto low = (((pixel & lanes) * factor) >> 8U) & lanes;
        const auto high = (((pixel >> 8U) & lanes) * factor) & ~lanes;
        result[index] = source[index] + (low | high);
    }
}

} // namespace ilemu
