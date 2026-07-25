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
    [[nodiscard]] std::uint32_t allocate_identifier();
    [[nodiscard]] std::uint64_t publication_watermark() const;
    [[nodiscard]] bool publish(AddressSpace& memory, Backing backing);
    [[nodiscard]] std::optional<SharedMapping>
    shared_mapping(std::uint32_t id) const;
    [[nodiscard]] std::optional<Backing> import(AddressSpace& memory,
                                                std::uint32_t id,
                                                std::uint32_t mapping_address);
    void erase(std::uint32_t id);
    [[nodiscard]] std::optional<Backing> find(std::uint32_t id) const;
    [[nodiscard]] std::optional<std::vector<std::uint32_t>>
    read_argb(AddressSpace& memory, std::uint32_t id) const;
    [[nodiscard]] bool write_argb(AddressSpace& memory, std::uint32_t id,
                                  std::span<const std::uint32_t> pixels) const;

  private:
    struct SharedObject {
        Backing metadata;
        std::uint32_t page_offset{};
        std::uint32_t mapping_size{};
        std::vector<std::shared_ptr<GuestPageBacking>> pages;
        std::size_t store_references{};
    };
    struct SharedRegistry {
        mutable std::mutex mutex;
        std::map<std::uint32_t, SharedObject> objects;
        std::uint32_t next_identifier{1};
        std::uint64_t next_publication_sequence{1};
        std::uint64_t publication_watermark{};
    };

    mutable std::mutex mutex_;
    std::map<std::uint32_t, Backing> backings_;
    std::shared_ptr<SharedRegistry> registry_{
        std::make_shared<SharedRegistry>()};
};

} // namespace ilemu
