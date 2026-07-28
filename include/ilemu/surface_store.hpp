#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

#include "ilemu/file_page_cache.hpp"

namespace ilemu {

class AddressSpace;
class HostSurface;
struct HostRectangle;

constexpr std::uint32_t surface_fourcc(char a, char b, char c, char d) {
    return (static_cast<std::uint32_t>(a) << 24U) |
           (static_cast<std::uint32_t>(b) << 16U) |
           (static_cast<std::uint32_t>(c) << 8U) |
           static_cast<std::uint32_t>(d);
}

inline constexpr std::uint32_t surface_pixel_format_bgra =
    surface_fourcc('B', 'G', 'R', 'A');
inline constexpr std::uint32_t surface_pixel_format_rgb555 =
    surface_fourcc('R', 'G', '1', '5');
// Public CoreSurface client images use 'L555' for the same little-endian,
// opaque RGB555 layout exposed to MBX clients as 'RG15'. Keep both names so
// imported metadata still reports the format chosen by the firmware.
inline constexpr std::uint32_t surface_pixel_format_rgb555_le =
    surface_fourcc('L', '5', '5', '5');
// Legacy CoreSurface's little-endian ARGB1555 client-image format. The
// firmware names it 's551': bit 15 is alpha and bits 14:0 are RGB555.
inline constexpr std::uint32_t surface_pixel_format_argb1555 =
    surface_fourcc('s', '5', '5', '1');

constexpr bool surface_is_rgb555(std::uint32_t pixel_format) {
    return pixel_format == surface_pixel_format_rgb555 ||
           pixel_format == surface_pixel_format_rgb555_le;
}

constexpr std::uint32_t
surface_bytes_per_pixel(std::uint32_t pixel_format) {
    if (pixel_format == surface_pixel_format_bgra)
        return 4U;
    if (surface_is_rgb555(pixel_format) ||
        pixel_format == surface_pixel_format_argb1555) {
        return 2U;
    }
    return 0U;
}

// Each process owns one SurfaceStore with process-local virtual addresses.
// Stores inherited across fork/spawn share a registry of page backings so a
// CoreSurface transport ID can be imported into a different guest address
// space without assuming that both tasks chose the same virtual address.
class SurfaceStore {
  public:
    ~SurfaceStore();

    struct Provenance {
        // The task that first published the backing. Importers retain this
        // identity instead of replacing it with the compositor's task.
        std::uint32_t producer_process_id{};
        // Assigned by the shared registry at publication time so reused
        // process or transport identifiers still describe distinct surfaces.
        std::uint64_t publication_sequence{};
    };

    struct Backing {
        std::uint32_t id{};
        std::uint32_t base{};
        std::uint32_t allocation_size{};
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t bytes_per_row{};
        std::uint32_t pixel_format{};
        Provenance provenance;
    };

    struct SharedMapping {
        Backing metadata;
        std::uint32_t mapping_size{};
    };

    void reset();
    void inherit_state(const SurfaceStore& parent);
    // A fresh exec address space has no process-local CoreSurface mappings,
    // but transport identifiers remain global kernel objects. Share only the
    // registry; callers import individual mappings through Lookup as firmware
    // passes their IDs between tasks.
    void share_registry(const SurfaceStore& peer);
    [[nodiscard]] std::uint32_t allocate_identifier();
    [[nodiscard]] std::uint64_t publication_watermark() const;
    [[nodiscard]] bool publish(AddressSpace& memory, Backing backing);
    [[nodiscard]] std::optional<SharedMapping>
    shared_mapping(std::uint32_t id) const;
    [[nodiscard]] std::optional<Backing> import(AddressSpace& memory,
                                                std::uint32_t id,
                                                std::uint32_t mapping_address);
    [[nodiscard]] std::optional<Backing>
    import(AddressSpace& memory, const SharedMapping& expected,
           std::uint32_t mapping_address,
           std::uint64_t* mapping_lease_token);
    void erase(std::uint32_t id);
    [[nodiscard]] std::optional<Backing> find(std::uint32_t id) const;
    [[nodiscard]] std::shared_ptr<HostSurface>
    host_surface(std::uint32_t id) const;
    [[nodiscard]] std::optional<std::vector<std::uint32_t>>
    read_argb(AddressSpace& memory, std::uint32_t id) const;
    // CPU map is the explicit GPU completion/readback boundary. The resulting
    // pixels are copied into the guest mapping for firmware access.
    [[nodiscard]] bool synchronize_for_cpu(AddressSpace& memory,
                                           std::uint32_t id,
                                           bool avoid_sync = false) const;
    // Imports guest CPU writes even when a newer GPU generation exists. This
    // is the unlock/flush direction, not a GPU readback request.
    [[nodiscard]] bool synchronize_from_guest(AddressSpace& memory,
                                              std::uint32_t id) const;
    [[nodiscard]] bool write_argb(AddressSpace& memory, std::uint32_t id,
                                  std::span<const std::uint32_t> pixels) const;

  private:
    struct SyncState {
        std::mutex mutex;
        std::vector<std::uint64_t> shared_page_generations;
        std::vector<std::uint32_t> guest_pixel_snapshot;
    };
    struct SharedObject {
        Backing metadata;
        std::uint32_t page_offset{};
        std::uint32_t mapping_size{};
        std::vector<std::shared_ptr<GuestPageBacking>> pages;
        std::shared_ptr<HostSurface> host_surface;
        std::shared_ptr<SyncState> sync_state;
        std::size_t store_references{};
    };
    struct SharedRegistry {
        mutable std::mutex mutex;
        std::map<std::uint32_t, SharedObject> objects;
        std::uint32_t next_identifier{1};
        std::uint64_t publication_watermark{};
    };

    [[nodiscard]] std::optional<std::vector<std::uint32_t>>
    read_guest_argb(AddressSpace& memory, const Backing& backing) const;
    [[nodiscard]] std::optional<std::vector<std::uint32_t>>
    read_guest_argb_region(AddressSpace& memory, const Backing& backing,
                           HostRectangle rectangle) const;
    [[nodiscard]] bool write_argb_region_to_guest(
        AddressSpace& memory, const Backing& backing, HostRectangle rectangle,
        std::span<const std::uint32_t> pixels) const;
    [[nodiscard]] std::shared_ptr<SyncState> shared_sync_state(std::uint32_t id) const;
    void update_guest_sync_generation(AddressSpace& memory,
                                      const Backing& backing) const;
    void update_guest_snapshot(const Backing& backing, HostRectangle rectangle,
                               std::span<const std::uint32_t> pixels) const;

    mutable std::mutex mutex_;
    std::map<std::uint32_t, Backing> backings_;
    std::shared_ptr<SharedRegistry> registry_{
        std::make_shared<SharedRegistry>()};
};

} // namespace ilemu
