#include "ilemu/mbx2d_hle.hpp"

#include <cstdint>
#include <limits>

namespace ilemu {

std::uint32_t Mbx2dHle::allocate_client_surface(
    std::uint32_t base, std::uint32_t allocation_size,
    std::uint32_t width) {
    if (base == 0 || width == 0 ||
        width > std::numeric_limits<std::uint32_t>::max() / 2U ||
        allocation_size < width * 2U ||
        allocation_size - 1U >
            std::numeric_limits<std::uint32_t>::max() - base) {
        return 0;
    }
    const auto handle = next_surface_++;
    surfaces_.emplace(
        handle,
        Surface{
            handle, 0, false,
            SurfaceStore::Backing{
                0, base, allocation_size, width, 0, 0,
                0, {}}});
    return handle;
}

}  // namespace ilemu
