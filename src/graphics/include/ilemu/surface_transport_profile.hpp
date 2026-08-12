#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ilemu::surface_transport {

// Firmware-facing user-space surface transports.  Selection follows the
// private symbol family exported by the loaded framework, never an OS build,
// application, or page.  Both profiles publish the same SurfaceStore backing
// while preserving their own client-object layout for native firmware code.
enum class Kind : std::uint8_t {
  CoreSurfaceClientBuffer,
  IOSurfaceClient,
};

struct Profile {
  std::string_view name;
  std::string_view image_suffix;
  std::string_view symbol_prefix;
  std::uint32_t public_client_pointer_offset;
  std::uint32_t client_structure_size;
  std::uint32_t reference_count_offset;
  std::uint32_t identifier_offset;
  std::uint32_t base_address_offset;
  std::uint32_t allocation_size_offset;
  std::uint32_t width_offset;
  std::uint32_t height_offset;
  std::uint32_t bytes_per_row_offset;
  std::uint32_t data_offset_offset;
  std::uint32_t pixel_format_offset;
  std::uint32_t plane_count_offset;
  bool lock_seed_output;
  std::array<std::string_view, 7> create_property_symbols;
};

inline constexpr Profile core_surface_client_buffer{
    .name = "core-surface-client-buffer",
    .image_suffix = "/CoreSurface.framework/CoreSurface",
    .symbol_prefix = "_CoreSurfaceClientBuffer",
    .public_client_pointer_offset = 8,
    .client_structure_size = 432,
    .reference_count_offset = 0,
    .identifier_offset = 4,
    .base_address_offset = 8,
    .allocation_size_offset = 12,
    .width_offset = 16,
    .height_offset = 20,
    .bytes_per_row_offset = 24,
    .data_offset_offset = 28,
    .pixel_format_offset = 32,
    .plane_count_offset = 40,
    .lock_seed_output = false,
    .create_property_symbols =
        {"_kCoreSurfaceBufferClientAddress",
         "_kCoreSurfaceBufferAllocSize",
         "_kCoreSurfaceBufferWidth",
         "_kCoreSurfaceBufferHeight",
         "_kCoreSurfaceBufferPitch",
         "_kCoreSurfaceBufferPixelFormat",
         "_kCoreSurfaceBufferOffset"},
};

// iPhoneOS builds with a separate IOSurface framework keep a 1,216-byte
// private client object.  CoreSurface remains a native compatibility wrapper
// and forwards into this symbol family.
inline constexpr Profile io_surface_client{
    .name = "io-surface-client",
    .image_suffix = "/IOSurface.framework/IOSurface",
    .symbol_prefix = "_IOSurfaceClient",
    .public_client_pointer_offset = 8,
    .client_structure_size = 1216,
    .reference_count_offset = 0,
    .identifier_offset = 12,
    .base_address_offset = 8,
    .allocation_size_offset = 16,
    .width_offset = 20,
    .height_offset = 24,
    .bytes_per_row_offset = 28,
    .data_offset_offset = 32,
    .pixel_format_offset = 36,
    .plane_count_offset = 44,
    .lock_seed_output = true,
    .create_property_symbols =
        {"",
         "_kIOSurfaceAllocSize",
         "_kIOSurfaceWidth",
         "_kIOSurfaceHeight",
         "_kIOSurfaceBytesPerRow",
         "_kIOSurfacePixelFormat",
         "_kIOSurfaceOffset"},
};

[[nodiscard]] constexpr const Profile &for_kind(Kind kind) {
  return kind == Kind::IOSurfaceClient ? io_surface_client
                                       : core_surface_client_buffer;
}

} // namespace ilemu::surface_transport
