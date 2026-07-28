#include "ilemu/surface_store.hpp"

#include <algorithm>

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/core_surface_abi.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/host_graphics.hpp"

namespace ilemu {
namespace {

std::atomic<std::uint64_t> next_host_surface_sequence{1};

std::uint64_t allocate_host_surface_sequence() {
    auto sequence =
        next_host_surface_sequence.fetch_add(1, std::memory_order_relaxed);
    if (sequence == 0) {
        sequence =
            next_host_surface_sequence.fetch_add(1,
                                                 std::memory_order_relaxed);
    }
    return sequence;
}

void normalize_written_ranges(std::vector<AddressSpace::WrittenRange>& ranges) {
    std::ranges::sort(ranges, {}, [](const AddressSpace::WrittenRange& range) {
        return range.address;
    });
    std::vector<AddressSpace::WrittenRange> normalized;
    normalized.reserve(ranges.size());
    for (const auto range : ranges) {
        if (range.size == 0) continue;
        if (normalized.empty()) {
            normalized.push_back(range);
            continue;
        }
        auto& previous = normalized.back();
        const auto previous_end =
            static_cast<std::uint64_t>(previous.address) + previous.size;
        const auto range_end = static_cast<std::uint64_t>(range.address) + range.size;
        if (range.address > previous_end) {
            normalized.push_back(range);
            continue;
        }
        previous.size = static_cast<std::uint32_t>(std::max(previous_end, range_end) -
                                                   previous.address);
    }
    ranges = std::move(normalized);
}

} // namespace

SurfaceStore::~SurfaceStore() {
    reset();
}

void SurfaceStore::reset() {
    const auto registry = registry_;
    std::vector<GlesRenderTargetKey> released_targets;
    {
        std::scoped_lock lock{mutex_, registry->mutex};
        for (const auto& [id, backing] : backings_) {
            static_cast<void>(backing);
            const auto object = registry->objects.find(id);
            if (object == registry->objects.end())
                continue;
            if (object->second.store_references > 1) {
                --object->second.store_references;
            } else {
                released_targets.push_back(
                    {0, object->second.metadata.provenance.publication_sequence});
                registry->objects.erase(object);
            }
        }
        backings_.clear();
    }
    release_gles_render_targets(released_targets);
}

void SurfaceStore::inherit_state(const SurfaceStore& parent) {
    if (this == &parent)
        return;
    reset();

    std::map<std::uint32_t, Backing> inherited;
    std::shared_ptr<SharedRegistry> inherited_registry;
    {
        std::lock_guard parent_lock{parent.mutex_};
        inherited_registry = parent.registry_;
        std::lock_guard registry_lock{inherited_registry->mutex};
        inherited = parent.backings_;
        for (const auto& [id, backing] : inherited) {
            static_cast<void>(backing);
            const auto object = inherited_registry->objects.find(id);
            if (object != inherited_registry->objects.end())
                ++object->second.store_references;
        }
    }
    {
        std::lock_guard lock{mutex_};
        backings_ = std::move(inherited);
        registry_ = std::move(inherited_registry);
    }
}

void SurfaceStore::share_registry(const SurfaceStore& peer) {
    if (this == &peer)
        return;

    std::shared_ptr<SharedRegistry> shared;
    {
        std::lock_guard peer_lock{peer.mutex_};
        shared = peer.registry_;
    }
    reset();
    std::lock_guard lock{mutex_};
    registry_ = std::move(shared);
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

    const auto registry = registry_;
    const auto published_id = backing.id;
    {
        std::scoped_lock lock{mutex_, registry->mutex};
        if (registry->objects.contains(backing.id) ||
            backings_.contains(backing.id)) {
            return false;
        }
        backing.provenance.publication_sequence =
            allocate_host_surface_sequence();
        registry->publication_watermark =
            backing.provenance.publication_sequence;
        SharedObject object;
        object.metadata = backing;
        object.metadata.base = 0;
        object.page_offset = page_offset;
        object.mapping_size = mapping_size;
        object.pages = std::move(*pages);
        object.host_surface = shared_gles_renderer()->create_surface(
            {0, backing.provenance.publication_sequence},
            HostSurfaceDescriptor{backing.width, backing.height,
                                  backing.bytes_per_row,
                                  backing.pixel_format,
                                  PerfSurfaceKind::CoreSurface});
        object.sync_state = std::make_shared<SyncState>();
        object.store_references = 1;
        registry->objects.emplace(backing.id, std::move(object));
        if (registry->next_identifier <= backing.id)
            registry->next_identifier = backing.id + 1U;
        backings_.insert_or_assign(backing.id, std::move(backing));
    }
    if (const auto pixels = read_argb(memory, published_id)) {
        if (const auto surface = host_surface(published_id))
            surface->replace_cpu(*pixels);
        if (const auto published = find(published_id)) {
            update_guest_snapshot(
                *published, HostRectangle{0, 0, published->width, published->height},
                *pixels);
        }
    }
    if (const auto published = find(published_id))
        update_guest_sync_generation(memory, *published);
    return true;
}

std::optional<SurfaceStore::SharedMapping>
SurfaceStore::shared_mapping(std::uint32_t id) const {
    const auto registry = registry_;
    std::lock_guard lock{registry->mutex};
    const auto found = registry->objects.find(id);
    if (found == registry->objects.end())
        return std::nullopt;
    return SharedMapping{found->second.metadata, found->second.mapping_size};
}

std::optional<SurfaceStore::Backing>
SurfaceStore::import(AddressSpace& memory, std::uint32_t id,
                     std::uint32_t mapping_address) {
    const auto expected = shared_mapping(id);
    if (!expected)
        return std::nullopt;
    return import(memory, *expected, mapping_address, nullptr);
}

std::optional<SurfaceStore::Backing>
SurfaceStore::import(AddressSpace& memory, const SharedMapping& expected,
                     std::uint32_t mapping_address,
                     std::uint64_t* mapping_lease_token) {
    if (mapping_lease_token)
        *mapping_lease_token = 0;
    const auto registry = registry_;
    std::scoped_lock lock{mutex_, registry->mutex};
    const auto id = expected.metadata.id;
    if (const auto local = backings_.find(id); local != backings_.end()) {
        if (mapping_lease_token)
            return std::nullopt;
        return local->second;
    }
    const auto found = registry->objects.find(id);
    if (found == registry->objects.end())
        return std::nullopt;
    const auto& object = found->second;
    if (object.metadata.provenance.publication_sequence !=
            expected.metadata.provenance.publication_sequence ||
        object.mapping_size != expected.mapping_size ||
        mapping_address == 0 ||
        mapping_address % AddressSpace::page_size != 0 ||
        !memory.map_page_backings(
            mapping_address, object.mapping_size,
            MemoryPermission::Read | MemoryPermission::Write, object.pages,
            AddressSpace::PageMappingMode::Shared, mapping_lease_token)) {
        return std::nullopt;
    }
    auto local = object.metadata;
    local.base = mapping_address + object.page_offset;
    backings_.insert_or_assign(id, local);
    ++found->second.store_references;
    return local;
}

void SurfaceStore::erase(std::uint32_t id) {
    const auto registry = registry_;
    std::optional<std::uint64_t> released_target;
    {
        std::scoped_lock lock{mutex_, registry->mutex};
        if (backings_.erase(id) == 0) return;
        const auto object = registry->objects.find(id);
        if (object == registry->objects.end())
            return;
        if (object->second.store_references > 1) {
            --object->second.store_references;
        } else {
            released_target =
                object->second.metadata.provenance.publication_sequence;
            registry->objects.erase(object);
        }
    }
    if (released_target)
        release_gles_render_target({0, *released_target});
}

std::optional<SurfaceStore::Backing>
SurfaceStore::find(std::uint32_t id) const {
    std::lock_guard lock{mutex_};
    const auto found = backings_.find(id);
    return found == backings_.end() ? std::nullopt
                                    : std::optional<Backing>{found->second};
}

std::shared_ptr<HostSurface>
SurfaceStore::host_surface(std::uint32_t id) const {
    const auto registry = registry_;
    std::lock_guard lock{registry->mutex};
    const auto found = registry->objects.find(id);
    return found == registry->objects.end() ? nullptr
                                            : found->second.host_surface;
}

std::shared_ptr<SurfaceStore::SyncState>
SurfaceStore::shared_sync_state(std::uint32_t id) const {
    const auto registry = registry_;
    std::lock_guard lock{registry->mutex};
    const auto found = registry->objects.find(id);
    return found == registry->objects.end() ? nullptr : found->second.sync_state;
}

std::optional<std::vector<std::uint32_t>>
SurfaceStore::read_argb(AddressSpace& memory, std::uint32_t id) const {
    const auto backing = find(id);
    if (!backing || backing->pixel_format != surface_pixel_format_bgra) {
        return std::nullopt;
    }
    const auto surface = host_surface(id);
    if (surface &&
        surface->gpu_generation() > surface->cpu_generation()) {
        if (!shared_gles_renderer()->map_cpu(
                *surface, true, PerfCpuMapReason::CoreSurface))
            return std::nullopt;
        auto mapping =
            surface->map_cpu(false, PerfCpuMapReason::CoreSurface);
        return mapping.frame().pixels;
    }
    auto pixels = read_guest_argb(memory, *backing);
    if (pixels) {
        if (surface) surface->replace_cpu(*pixels);
        update_guest_snapshot(
            *backing, HostRectangle{0, 0, backing->width, backing->height}, *pixels);
        update_guest_sync_generation(memory, *backing);
    }
    return pixels;
}

std::optional<std::vector<std::uint32_t>>
SurfaceStore::read_guest_argb(AddressSpace& memory,
                              const Backing& backing) const {
    constexpr auto pixel_size = core_surface_abi::bytes_per_bgra_pixel;
    const auto row_bytes =
        static_cast<std::uint64_t>(backing.width) * pixel_size;
    if (row_bytes > backing.bytes_per_row)
        return std::nullopt;
    const auto required =
        backing.height == 0
            ? 0
            : static_cast<std::uint64_t>(backing.height - 1U) *
                      backing.bytes_per_row +
                  row_bytes;
    if (required > backing.allocation_size ||
        required > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    const auto source =
        memory.read_bytes(backing.base, static_cast<std::size_t>(required));
    if (!source)
        return std::nullopt;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(backing.width) *
                                      backing.height);
    for (std::uint32_t y = 0; y < backing.height; ++y) {
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(pixels.data() +
                            static_cast<std::size_t>(y) * backing.width,
                        source->data() + static_cast<std::size_t>(y) *
                                             backing.bytes_per_row,
                        static_cast<std::size_t>(row_bytes));
            continue;
        }
        for (std::uint32_t x = 0; x < backing.width; ++x) {
            const auto offset = static_cast<std::size_t>(
                static_cast<std::uint64_t>(y) * backing.bytes_per_row +
                static_cast<std::uint64_t>(x) * pixel_size);
            const auto blue = std::to_integer<std::uint32_t>((*source)[offset]);
            const auto green =
                std::to_integer<std::uint32_t>((*source)[offset + 1U]);
            const auto red =
                std::to_integer<std::uint32_t>((*source)[offset + 2U]);
            const auto alpha =
                std::to_integer<std::uint32_t>((*source)[offset + 3U]);
            pixels[static_cast<std::size_t>(y) * backing.width + x] =
                (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
        }
    }
    return pixels;
}

std::optional<std::vector<std::uint32_t>>
SurfaceStore::read_guest_argb_region(
    AddressSpace& memory, const Backing& backing,
    HostRectangle rectangle) const {
    constexpr auto pixel_size =
        core_surface_abi::bytes_per_bgra_pixel;
    if (rectangle.x < 0 || rectangle.y < 0 ||
        rectangle.width == 0 || rectangle.height == 0 ||
        rectangle.width > backing.width ||
        rectangle.height > backing.height ||
        static_cast<std::uint32_t>(rectangle.x) >
            backing.width - rectangle.width ||
        static_cast<std::uint32_t>(rectangle.y) >
            backing.height - rectangle.height) {
        return std::nullopt;
    }
    const auto row_bytes =
        static_cast<std::uint64_t>(rectangle.width) * pixel_size;
    const auto visible_row_bytes =
        static_cast<std::uint64_t>(backing.width) * pixel_size;
    if (visible_row_bytes > backing.bytes_per_row ||
        row_bytes > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(rectangle.width) *
        rectangle.height);
    for (std::uint32_t row = 0; row < rectangle.height; ++row) {
        const auto source_y =
            static_cast<std::uint32_t>(rectangle.y) + row;
        const auto offset =
            static_cast<std::uint64_t>(source_y) *
                backing.bytes_per_row +
            static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(rectangle.x)) *
                pixel_size;
        if (offset > backing.allocation_size ||
            row_bytes > backing.allocation_size - offset ||
            offset + row_bytes >
                std::numeric_limits<std::uint32_t>::max() ||
            backing.base >
                std::numeric_limits<std::uint32_t>::max() -
                    (offset + row_bytes)) {
            return std::nullopt;
        }
        const auto bytes = memory.read_bytes(
            backing.base + static_cast<std::uint32_t>(offset),
            static_cast<std::size_t>(row_bytes));
        if (!bytes)
            return std::nullopt;
        if constexpr (std::endian::native ==
                      std::endian::little) {
            std::memcpy(
                pixels.data() +
                    static_cast<std::size_t>(row) *
                        rectangle.width,
                bytes->data(), static_cast<std::size_t>(row_bytes));
        } else {
            for (std::uint32_t x = 0; x < rectangle.width; ++x) {
                const auto byte =
                    static_cast<std::size_t>(x) * pixel_size;
                const auto blue =
                    std::to_integer<std::uint32_t>((*bytes)[byte]);
                const auto green = std::to_integer<std::uint32_t>(
                    (*bytes)[byte + 1U]);
                const auto red =
                    std::to_integer<std::uint32_t>(
                        (*bytes)[byte + 2U]);
                const auto alpha = std::to_integer<std::uint32_t>(
                    (*bytes)[byte + 3U]);
                pixels[static_cast<std::size_t>(row) *
                           rectangle.width +
                       x] =
                    (alpha << 24U) | (red << 16U) |
                    (green << 8U) | blue;
            }
        }
    }
    return pixels;
}

bool SurfaceStore::synchronize_for_cpu(AddressSpace& memory,
                                       std::uint32_t id,
                                       bool avoid_sync) const {
    const auto backing = find(id);
    if (!backing)
        return false;
    if (backing->pixel_format != surface_pixel_format_bgra)
        return true;
    const auto surface = host_surface(id);
    if (!surface || surface->gpu_generation() <= surface->cpu_generation())
        return true;
    // AvoidSync accepts ownership without waiting for a GPU readback. It is a
    // synchronization policy, not a try-lock flag, so the firmware lock still
    // succeeds when native contents are newer.
    if (avoid_sync) return true;

    const auto sync_state = shared_sync_state(id);
    if (!sync_state) return false;
    // Both synchronization directions serialize the shared guest snapshot
    // before touching the host surface.  Keeping one lock order avoids a
    // GPU-readback/guest-flush deadlock when different guest tasks use the
    // same CoreSurface concurrently.
    std::unique_lock sync_lock{sync_state->mutex};

    std::optional<HostRectangle> damage;
    if (!shared_gles_renderer()->map_cpu(
            *surface, true, PerfCpuMapReason::CoreSurface, &damage)) {
        return false;
    }
    if (!damage)
        return true;
    auto mapping = surface->map_cpu(false, PerfCpuMapReason::CoreSurface);
    if (!write_argb_region_to_guest(
            memory, *backing, *damage, mapping.frame().pixels)) {
        return false;
    }
    const auto pixel_count =
        static_cast<std::size_t>(backing->width) * backing->height;
    auto& snapshot = sync_state->guest_pixel_snapshot;
    if (snapshot.size() != pixel_count) {
        snapshot = mapping.frame().pixels;
    } else {
        for (std::uint32_t row = 0; row < damage->height; ++row) {
            const auto offset =
                (static_cast<std::size_t>(
                     static_cast<std::uint32_t>(damage->y) + row) *
                 backing->width) +
                static_cast<std::uint32_t>(damage->x);
            std::copy_n(
                mapping.frame().pixels.begin() +
                    static_cast<std::ptrdiff_t>(offset),
                damage->width,
                snapshot.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    const auto shared_generation = memory.shared_write_generation_changes(
        backing->base, backing->allocation_size);
    if (shared_generation) {
        sync_state->shared_page_generations =
            shared_generation->page_generations;
    }
    return true;
}

bool SurfaceStore::synchronize_from_guest(AddressSpace& memory,
                                          std::uint32_t id) const {
    const auto backing = find(id);
    if (!backing || backing->pixel_format != surface_pixel_format_bgra)
        return backing.has_value();
    const auto sync_state = shared_sync_state(id);
    if (!sync_state) return false;
    std::unique_lock sync_lock{sync_state->mutex};
    const auto shared_changes = memory.shared_write_generation_changes(
        backing->base, backing->allocation_size, sync_state->shared_page_generations);
    if (!shared_changes) return false;
    auto written_ranges = shared_changes->ranges;
    normalize_written_ranges(written_ranges);
    if (written_ranges.empty()) {
        sync_state->shared_page_generations = shared_changes->page_generations;
        return true;
    }

    constexpr auto pixel_size =
        core_surface_abi::bytes_per_bgra_pixel;
    const auto visible_row_bytes =
        static_cast<std::uint64_t>(backing->width) * pixel_size;
    if (backing->bytes_per_row == 0 ||
        visible_row_bytes > backing->bytes_per_row) {
        return false;
    }
    std::vector<HostRectangle> rectangles;
    for (const auto& range : written_ranges) {
        const auto range_begin =
            static_cast<std::uint64_t>(range.address) -
            backing->base;
        const auto range_end = range_begin + range.size;
        const auto first_row =
            range_begin / backing->bytes_per_row;
        const auto end_row =
            (range_end + backing->bytes_per_row - 1U) /
            backing->bytes_per_row;
        for (auto row = first_row;
             row < end_row && row < backing->height; ++row) {
            const auto row_begin = row * backing->bytes_per_row;
            const auto visible_end = row_begin + visible_row_bytes;
            const auto dirty_begin =
                std::max(range_begin, row_begin);
            const auto dirty_end =
                std::min(range_end, visible_end);
            if (dirty_end <= dirty_begin)
                continue;
            const auto x =
                (dirty_begin - row_begin) / pixel_size;
            const auto x_end =
                (dirty_end - row_begin + pixel_size - 1U) /
                pixel_size;
            if (x_end <= x || x >= backing->width)
                continue;
            const auto rectangle = HostRectangle{
                static_cast<std::int32_t>(x),
                static_cast<std::int32_t>(row),
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        x_end, backing->width) -
                    x),
                1};
            if (!rectangles.empty() &&
                rectangles.back().x == rectangle.x &&
                rectangles.back().width == rectangle.width &&
                static_cast<std::uint32_t>(
                    rectangles.back().y) +
                        rectangles.back().height ==
                    static_cast<std::uint32_t>(rectangle.y)) {
                ++rectangles.back().height;
            } else {
                rectangles.push_back(rectangle);
            }
        }
    }
    struct GuestRegion {
        HostRectangle rectangle;
        std::vector<std::uint32_t> pixels;
    };
    std::vector<GuestRegion> guest_regions;
    guest_regions.reserve(rectangles.size());
    for (const auto rectangle : rectangles) {
        auto pixels = read_guest_argb_region(memory, *backing, rectangle);
        if (!pixels) return false;
        guest_regions.push_back(GuestRegion{rectangle, std::move(*pixels)});
    }

    struct ChangedRun {
        HostRectangle rectangle;
        std::vector<std::uint32_t> pixels;
    };
    std::vector<ChangedRun> changed_runs;
    const auto expected_pixels =
        static_cast<std::size_t>(backing->width) * backing->height;
    if (sync_state->guest_pixel_snapshot.size() != expected_pixels) {
        const auto surface = host_surface(id);
        if (surface && surface->gpu_generation() > surface->cpu_generation()) {
            return false;
        }
        const auto pixels = read_guest_argb(memory, *backing);
        if (!pixels) return false;
        if (surface) surface->replace_cpu(*pixels);
        sync_state->guest_pixel_snapshot = *pixels;
        sync_state->shared_page_generations = shared_changes->page_generations;
        return true;
    }
    for (const auto& region : guest_regions) {
        for (std::uint32_t row = 0; row < region.rectangle.height; ++row) {
            const auto source_row =
                static_cast<std::size_t>(row) * region.rectangle.width;
            const auto snapshot_row =
                (static_cast<std::size_t>(
                     static_cast<std::uint32_t>(region.rectangle.y) + row) *
                 backing->width) +
                static_cast<std::uint32_t>(region.rectangle.x);
            std::uint32_t x = 0;
            while (x < region.rectangle.width) {
                while (x < region.rectangle.width &&
                       region.pixels[source_row + x] ==
                           sync_state->guest_pixel_snapshot[snapshot_row + x]) {
                    ++x;
                }
                const auto begin = x;
                while (x < region.rectangle.width &&
                       region.pixels[source_row + x] !=
                           sync_state->guest_pixel_snapshot[snapshot_row + x]) {
                    ++x;
                }
                if (x == begin) continue;
                ChangedRun run;
                run.rectangle = HostRectangle{
                    region.rectangle.x + static_cast<std::int32_t>(begin),
                    region.rectangle.y + static_cast<std::int32_t>(row), x - begin, 1};
                run.pixels.assign(region.pixels.begin() +
                                      static_cast<std::ptrdiff_t>(source_row + begin),
                                  region.pixels.begin() +
                                      static_cast<std::ptrdiff_t>(source_row + x));
                changed_runs.push_back(std::move(run));
            }
        }
    }

    const auto surface = host_surface(id);
    if (!changed_runs.empty()) {
        if (!surface) return false;
        if (surface->gpu_generation() > surface->cpu_generation() &&
            !shared_gles_renderer()->map_cpu(*surface, true,
                                             PerfCpuMapReason::CoreSurface)) {
            return false;
        }
        auto mapping = surface->map_cpu(true, PerfCpuMapReason::CoreSurface);
        std::optional<HostRectangle> damage;
        for (const auto& run : changed_runs) {
            const auto destination =
                static_cast<std::size_t>(static_cast<std::uint32_t>(run.rectangle.y)) *
                    backing->width +
                static_cast<std::uint32_t>(run.rectangle.x);
            std::copy(run.pixels.begin(), run.pixels.end(),
                      mapping.frame().pixels.begin() +
                          static_cast<std::ptrdiff_t>(destination));
            if (!damage) {
                damage = run.rectangle;
                continue;
            }
            const auto left = std::min(damage->x, run.rectangle.x);
            const auto top = std::min(damage->y, run.rectangle.y);
            const auto right = std::max(
                static_cast<std::int64_t>(damage->x) + damage->width,
                static_cast<std::int64_t>(run.rectangle.x) + run.rectangle.width);
            const auto bottom = std::max(
                static_cast<std::int64_t>(damage->y) + damage->height,
                static_cast<std::int64_t>(run.rectangle.y) + run.rectangle.height);
            damage = HostRectangle{left, top, static_cast<std::uint32_t>(right - left),
                                   static_cast<std::uint32_t>(bottom - top)};
        }
        if (damage) mapping.set_damage(*damage);
    }
    for (const auto& run : changed_runs) {
        const auto destination =
            static_cast<std::size_t>(static_cast<std::uint32_t>(run.rectangle.y)) *
                backing->width +
            static_cast<std::uint32_t>(run.rectangle.x);
        std::copy(run.pixels.begin(), run.pixels.end(),
                  sync_state->guest_pixel_snapshot.begin() +
                      static_cast<std::ptrdiff_t>(destination));
    }
    sync_state->shared_page_generations = shared_changes->page_generations;
    return true;
}

bool SurfaceStore::write_argb(AddressSpace& memory, std::uint32_t id,
                              std::span<const std::uint32_t> pixels) const {
    const auto backing = find(id);
    if (!backing || backing->pixel_format != surface_pixel_format_bgra) {
        return false;
    }
    const auto pixel_count =
        static_cast<std::uint64_t>(backing->width) * backing->height;
    if (pixel_count != pixels.size()) {
        return false;
    }
    if (!write_argb_region_to_guest(
            memory, *backing,
            HostRectangle{0, 0, backing->width, backing->height}, pixels)) {
        return false;
    }

    if (const auto surface = host_surface(id))
        surface->replace_cpu(pixels);
    update_guest_snapshot(*backing,
                          HostRectangle{0, 0, backing->width, backing->height}, pixels);
    update_guest_sync_generation(memory, *backing);
    return true;
}

void SurfaceStore::update_guest_sync_generation(
    AddressSpace& memory, const Backing& backing) const {
    const auto sync_state = shared_sync_state(backing.id);
    if (!sync_state) return;
    std::lock_guard sync_lock{sync_state->mutex};
    const auto shared_generation =
        memory.shared_write_generation_changes(backing.base, backing.allocation_size);
    if (!shared_generation) return;
    sync_state->shared_page_generations = shared_generation->page_generations;
}

void SurfaceStore::update_guest_snapshot(const Backing& backing,
                                         HostRectangle rectangle,
                                         std::span<const std::uint32_t> pixels) const {
    const auto pixel_count = static_cast<std::size_t>(backing.width) * backing.height;
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.width == 0 ||
        rectangle.height == 0 || rectangle.width > backing.width ||
        rectangle.height > backing.height ||
        static_cast<std::uint32_t>(rectangle.x) > backing.width - rectangle.width ||
        static_cast<std::uint32_t>(rectangle.y) > backing.height - rectangle.height ||
        pixels.size() != pixel_count) {
        return;
    }
    const auto sync_state = shared_sync_state(backing.id);
    if (!sync_state) return;
    std::lock_guard sync_lock{sync_state->mutex};
    auto& snapshot = sync_state->guest_pixel_snapshot;
    if (snapshot.size() != pixel_count) {
        snapshot.assign(pixels.begin(), pixels.end());
        return;
    }
    for (std::uint32_t row = 0; row < rectangle.height; ++row) {
        const auto offset =
            (static_cast<std::size_t>(static_cast<std::uint32_t>(rectangle.y) + row) *
             backing.width) +
            static_cast<std::uint32_t>(rectangle.x);
        std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>(offset),
                    rectangle.width,
                    snapshot.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

bool SurfaceStore::write_argb_region_to_guest(
    AddressSpace& memory, const Backing& backing, HostRectangle rectangle,
    std::span<const std::uint32_t> pixels) const {
    constexpr auto pixel_size = core_surface_abi::bytes_per_bgra_pixel;
    const auto pixel_count =
        static_cast<std::uint64_t>(backing.width) * backing.height;
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.width == 0 ||
        rectangle.height == 0 || rectangle.width > backing.width ||
        rectangle.height > backing.height ||
        static_cast<std::uint32_t>(rectangle.x) >
            backing.width - rectangle.width ||
        static_cast<std::uint32_t>(rectangle.y) >
            backing.height - rectangle.height ||
        pixel_count != pixels.size()) {
        return false;
    }
    const auto row_bytes =
        static_cast<std::uint64_t>(rectangle.width) * pixel_size;
    const auto last_row = static_cast<std::uint64_t>(rectangle.y) +
                          rectangle.height - 1U;
    const auto required = last_row * backing.bytes_per_row +
                          (static_cast<std::uint64_t>(rectangle.x) +
                           rectangle.width) *
                              pixel_size;
    if (required > backing.allocation_size ||
        required > std::numeric_limits<std::uint32_t>::max() ||
        backing.base > std::numeric_limits<std::uint32_t>::max() - required) {
        return false;
    }

    std::vector<std::byte> encoded_row;
    if constexpr (std::endian::native != std::endian::little) {
        encoded_row.resize(static_cast<std::size_t>(row_bytes));
    }
    for (std::uint32_t y = 0; y < rectangle.height; ++y) {
        const auto source_y = static_cast<std::uint32_t>(rectangle.y) + y;
        const auto row = pixels.subspan(
            static_cast<std::size_t>(source_y) * backing.width +
                static_cast<std::uint32_t>(rectangle.x),
            rectangle.width);
        std::span<const std::byte> bytes;
        if constexpr (std::endian::native == std::endian::little) {
            bytes = {reinterpret_cast<const std::byte*>(row.data()),
                     static_cast<std::size_t>(row_bytes)};
        } else {
            for (std::uint32_t x = 0; x < rectangle.width; ++x) {
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
        const auto destination =
            backing.base + source_y * backing.bytes_per_row +
            static_cast<std::uint32_t>(rectangle.x) * pixel_size;
        if (!memory.copy_in(destination, bytes))
            return false;
    }
    return true;
}

} // namespace ilemu
