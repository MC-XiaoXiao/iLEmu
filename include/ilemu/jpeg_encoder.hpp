#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ilemu {

// Host codec boundary used by hardware profiles that expose encoded image
// buffers to the guest. Input pixels are opaque 0xAARRGGBB words.
[[nodiscard]] std::optional<std::vector<std::byte>>
encode_jpeg_argb(std::span<const std::uint32_t> pixels, std::uint32_t width,
                 std::uint32_t height, int quality = 90);

} // namespace ilemu
