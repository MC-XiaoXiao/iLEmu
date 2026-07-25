#include "ilegacysim/surface_store.hpp"

#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "ilegacysim/address_space.hpp"
#include "ilegacysim/core_surface_abi.hpp"

namespace ilegacysim {

void SurfaceStore::reset() {
    std::lock_guard lock{mutex_};
    backings_.clear();
}

void SurfaceStore::inherit_state(const SurfaceStore& parent) {
    if (this == &parent)
        return;
    std::scoped_lock lock{mutex_, parent.mutex_};
    backings_ = parent.backings_;
    registry_ = parent.registry_;
}

std::uint32_t SurfaceStore::allocate_identifier() {
    std::lock_guard lock{registry_->mutex};
    while (registry_->next_identifier == 0 ||
           registry_->objects.contains(registry_->next_identifier)) {
        ++registry_->next_identifier;
    }
    return registry_->next_identifier++;
}

std::uint64_t SurfaceStore::publication_watermark() const {
    std::lock_guard lock{registry_->mutex};
    return registry_->publication_watermark;
}

bool SurfaceStore::publish(AddressSpace& memory, Backing backing) {
    if (backing.id == 0 || backing.base == 0 || backing.allocation_size == 0) {
        return false;
    }
    constexpr auto page_mask = AddressSpace::page_size - 1U;
    const auto mapping_address = backing.base & ~page_mask;
    const auto page_offset = backing.base - mapping_address;
    if (backing.allocation_size >
        std::numeric_limits<std::uint32_t>::max() - page_offset - page_mask) {
        return false;
    }
    const auto mapping_size =
        (backing.allocation_size + page_offset + page_mask) & ~page_mask;
    auto pages = memory.share_pages(mapping_address, mapping_size);
    if (!pages)
        return false;

    {
        std::lock_guard lock{registry_->mutex};
        if (registry_->objects.contains(backing.id))
            return false;
        backing.provenance.publication_sequence =
            registry_->next_publication_sequence++;
        registry_->publication_watermark =
            backing.provenance.publication_sequence;
        if (registry_->next_publication_sequence == 0U)
            registry_->next_publication_sequence = 1U;
        SharedObject object;
        object.metadata = backing;
        object.metadata.base = 0;
        object.page_offset = page_offset;
        object.mapping_size = mapping_size;
        object.pages = std::move(*pages);
        registry_->objects.emplace(backing.id, std::move(object));
        if (registry_->next_identifier <= backing.id)
            registry_->next_identifier = backing.id + 1U;
    }
    {
        std::lock_guard lock{mutex_};
        backings_.insert_or_assign(backing.id, std::move(backing));
    }
    return true;
}

std::optional<SurfaceStore::SharedMapping>
SurfaceStore::shared_mapping(std::uint32_t id) const {
    std::lock_guard lock{registry_->mutex};
    const auto found = registry_->objects.find(id);
    if (found == registry_->objects.end())
        return std::nullopt;
    return SharedMapping{found->second.metadata, found->second.mapping_size};
}

std::optional<SurfaceStore::Backing>
SurfaceStore::import(AddressSpace& memory, std::uint32_t id,
                     std::uint32_t mapping_address) {
    {
        std::lock_guard lock{mutex_};
        if (const auto local = backings_.find(id); local != backings_.end())
            return local->second;
    }

    SharedObject object;
    {
        std::lock_guard lock{registry_->mutex};
        const auto found = registry_->objects.find(id);
        if (found == registry_->objects.end())
            return std::nullopt;
        object = found->second;
    }
    if (mapping_address == 0 ||
        mapping_address % AddressSpace::page_size != 0 ||
        !memory.map_page_backings(
            mapping_address, object.mapping_size,
            MemoryPermission::Read | MemoryPermission::Write, object.pages,
            AddressSpace::PageMappingMode::Shared)) {
        return std::nullopt;
    }
    auto local = object.metadata;
    local.base = mapping_address + object.page_offset;
    {
        std::lock_guard lock{mutex_};
        backings_.insert_or_assign(id, local);
    }
    return local;
}

void SurfaceStore::erase(std::uint32_t id) {
    std::lock_guard lock{mutex_};
    backings_.erase(id);
}

std::optional<SurfaceStore::Backing>
SurfaceStore::find(std::uint32_t id) const {
    std::lock_guard lock{mutex_};
    const auto found = backings_.find(id);
    return found == backings_.end() ? std::nullopt
                                    : std::optional<Backing>{found->second};
}

std::optional<std::vector<std::uint32_t>>
SurfaceStore::read_argb(AddressSpace& memory, std::uint32_t id) const {
    const auto backing = find(id);
    if (!backing || backing->pixel_format != surface_pixel_format_bgra) {
        return std::nullopt;
    }
    constexpr auto pixel_size = core_surface_abi::bytes_per_bgra_pixel;
    const auto row_bytes =
        static_cast<std::uint64_t>(backing->width) * pixel_size;
    if (row_bytes > backing->bytes_per_row)
        return std::nullopt;
    const auto required =
        backing->height == 0
            ? 0
            : static_cast<std::uint64_t>(backing->height - 1U) *
                      backing->bytes_per_row +
                  row_bytes;
    if (required > backing->allocation_size ||
        required > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    const auto source =
        memory.read_bytes(backing->base, static_cast<std::size_t>(required));
    if (!source)
        return std::nullopt;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(backing->width) *
                                      backing->height);
    for (std::uint32_t y = 0; y < backing->height; ++y) {
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(pixels.data() +
                            static_cast<std::size_t>(y) * backing->width,
                        source->data() + static_cast<std::size_t>(y) *
                                             backing->bytes_per_row,
                        static_cast<std::size_t>(row_bytes));
            continue;
        }
        for (std::uint32_t x = 0; x < backing->width; ++x) {
            const auto offset = static_cast<std::size_t>(
                static_cast<std::uint64_t>(y) * backing->bytes_per_row +
                static_cast<std::uint64_t>(x) * pixel_size);
            const auto blue = std::to_integer<std::uint32_t>((*source)[offset]);
            const auto green =
                std::to_integer<std::uint32_t>((*source)[offset + 1U]);
            const auto red =
                std::to_integer<std::uint32_t>((*source)[offset + 2U]);
            const auto alpha =
                std::to_integer<std::uint32_t>((*source)[offset + 3U]);
            pixels[static_cast<std::size_t>(y) * backing->width + x] =
                (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
        }
    }
    return pixels;
}

bool SurfaceStore::write_argb(AddressSpace& memory, std::uint32_t id,
                              std::span<const std::uint32_t> pixels) const {
    const auto backing = find(id);
    if (!backing || backing->pixel_format != surface_pixel_format_bgra) {
        return false;
    }
    constexpr auto pixel_size = core_surface_abi::bytes_per_bgra_pixel;
    const auto row_bytes =
        static_cast<std::uint64_t>(backing->width) * pixel_size;
    const auto pixel_count =
        static_cast<std::uint64_t>(backing->width) * backing->height;
    if (row_bytes > backing->bytes_per_row || pixel_count != pixels.size()) {
        return false;
    }
    const auto required =
        backing->height == 0
            ? 0
            : static_cast<std::uint64_t>(backing->height - 1U) *
                      backing->bytes_per_row +
                  row_bytes;
    if (required > backing->allocation_size ||
        required > std::numeric_limits<std::uint32_t>::max() ||
        backing->base > std::numeric_limits<std::uint32_t>::max() - required) {
        return false;
    }

    std::vector<std::byte> encoded_row;
    if constexpr (std::endian::native != std::endian::little) {
        encoded_row.resize(static_cast<std::size_t>(row_bytes));
    }
    for (std::uint32_t y = 0; y < backing->height; ++y) {
        const auto row = pixels.subspan(
            static_cast<std::size_t>(y) * backing->width, backing->width);
        std::span<const std::byte> bytes;
        if constexpr (std::endian::native == std::endian::little) {
            bytes = {reinterpret_cast<const std::byte*>(row.data()),
                     static_cast<std::size_t>(row_bytes)};
        } else {
            for (std::uint32_t x = 0; x < backing->width; ++x) {
                const auto pixel = row[x];
                const auto offset = static_cast<std::size_t>(x) * pixel_size;
                encoded_row[offset] = static_cast<std::byte>(pixel & 0xffU);
                encoded_row[offset + 1U] =
                    static_cast<std::byte>((pixel >> 8U) & 0xffU);
                encoded_row[offset + 2U] =
                    static_cast<std::byte>((pixel >> 16U) & 0xffU);
                encoded_row[offset + 3U] =
                    static_cast<std::byte>((pixel >> 24U) & 0xffU);
            }
            bytes = encoded_row;
        }
        const auto destination = backing->base + y * backing->bytes_per_row;
        if (!memory.copy_in(destination, bytes))
            return false;
    }
    return true;
}

} // namespace ilegacysim
