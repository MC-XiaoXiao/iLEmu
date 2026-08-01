#include "ilemu/core_surface_hle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/core_surface_abi.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/display.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/output.hpp"
#include "ilemu/presentation_tracker.hpp"
#include "ilemu/scene_coordinator.hpp"
#include "ilemu/surface_store.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view core_surface_image{
    "/CoreSurface.framework/CoreSurface"};
constexpr std::string_view core_foundation_image{
    "/CoreFoundation.framework/CoreFoundation"};
constexpr std::string_view client_buffer_prefix{"_CoreSurfaceClientBuffer"};
constexpr std::string_view client_buffer_alloc{
    "_CoreSurfaceClientBufferAlloc"};
constexpr std::string_view client_buffer_wrap_image{
    "_CoreSurfaceClientBufferWrapClientImage"};
constexpr std::string_view client_buffer_wrap_image_transport{
    "__coreSurfaceClientBufferWrapClientImage"};

constexpr std::size_t client_buffer_alignment = 16;
constexpr std::size_t pixel_buffer_alignment = 64;
constexpr std::size_t maximum_unsupported_traces = 32;
constexpr std::uint64_t maximum_surface_bytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t cf_number_sint32_type = 3;

enum CreateProperty : std::size_t {
    client_address,
    allocation_size,
    width,
    height,
    pitch,
    pixel_format,
    data_offset,
};

constexpr std::array<std::string_view, 7> create_property_symbols{
    "_kCoreSurfaceBufferClientAddress", "_kCoreSurfaceBufferAllocSize",
    "_kCoreSurfaceBufferWidth",         "_kCoreSurfaceBufferHeight",
    "_kCoreSurfaceBufferPitch",         "_kCoreSurfaceBufferPixelFormat",
    "_kCoreSurfaceBufferOffset",
};

// Bytes in guest memory are B,G,R,A; when read as a little-endian uint32_t
// this is the DisplayState 0xAARRGGBB representation without conversion.
constexpr std::uint32_t success = 0;

} // namespace

CoreSurfaceHle::CoreSurfaceHle(
    UserlandHleRegistry& registry, std::shared_ptr<DisplayState> display,
    std::shared_ptr<SurfaceStore> surfaces,
    std::shared_ptr<PresentationTracker> presentations)
    : display_{std::move(display)},
      surfaces_{surfaces ? std::move(surfaces)
                         : std::make_shared<SurfaceStore>()},
      presentation_tracker_{std::move(presentations)} {
    registry.register_prefix(std::string{core_surface_image},
                             std::string{client_buffer_prefix},
                             [this](UserlandHleCall& call) { dispatch(call); });
    // These public helpers construct the firmware's client wrapper around a
    // transport identifier. Keep that native object lifecycle and intercept
    // only the private transport transaction that would otherwise require
    // the unavailable CoreSurface kernel user client.
    for (const auto symbol :
         {client_buffer_alloc, client_buffer_wrap_image}) {
        registry.register_function(
            std::string{core_surface_image}, std::string{symbol},
            [](UserlandHleCall& call) {
                call.resume_original_persistently();
            });
    }
    registry.register_function(
        std::string{core_surface_image},
        std::string{client_buffer_wrap_image_transport},
        [this](UserlandHleCall& call) { dispatch(call); });
    registry.register_guest_function(std::string{core_foundation_image},
                                     "_CFDictionaryGetValue");
    registry.register_guest_function(std::string{core_foundation_image},
                                     "_CFNumberGetValue");
}

void CoreSurfaceHle::reset() {
    buffers_.clear();
    clients_by_id_.clear();
    free_client_buffers_.clear();
    free_imported_mappings_.clear();
    unsupported_trace_count_ = 0;
    last_scanout_generation_.reset();
    last_scanout_pixels_.clear();
    surfaces_->reset();
}

void CoreSurfaceHle::inherit_state(const CoreSurfaceHle& parent) {
    buffers_ = parent.buffers_;
    clients_by_id_ = parent.clients_by_id_;
    free_client_buffers_ = parent.free_client_buffers_;
    free_imported_mappings_ = parent.free_imported_mappings_;
    last_scanout_generation_ = parent.last_scanout_generation_;
    last_scanout_pixels_ = parent.last_scanout_pixels_;
    presentation_tracker_ = parent.presentation_tracker_;
    surfaces_->inherit_state(*parent.surfaces_);
}

void CoreSurfaceHle::set_display(std::shared_ptr<DisplayState> display) {
    display_ = std::move(display);
}

void CoreSurfaceHle::set_presentation_tracker(
    std::shared_ptr<PresentationTracker> presentations) {
    presentation_tracker_ = std::move(presentations);
}

void CoreSurfaceHle::set_shared_state(
    std::shared_ptr<KernelSharedState> shared_state) {
    shared_state_ = std::move(shared_state);
}

void CoreSurfaceHle::set_scene_coordinator(
    std::shared_ptr<SceneCoordinator> scenes) {
    scene_coordinator_ = std::move(scenes);
}

bool CoreSurfaceHle::refresh_default_scanout(AddressSpace& memory,
                                             std::uint32_t owner_process_id) {
    if (!display_)
        return false;
    const auto backing =
        surfaces_->find(iokit_abi::apple_h1clcd_default_surface_id);
    if (!backing)
        return false;
    const auto generation =
        memory.range_write_generation(backing->base, backing->allocation_size);
    if (!generation || generation == last_scanout_generation_)
        return false;
    last_scanout_generation_ = generation;
    const auto pixels = surfaces_->read_argb(
        memory, iokit_abi::apple_h1clcd_default_surface_id);
    if (!pixels || pixels->empty() || *pixels == last_scanout_pixels_) {
        return false;
    }

    // A freshly allocated CoreSurface is zero-filled.  Do not replace the
    // display's opaque-black power-on frame until the firmware has actually
    // rendered something into the LCD backing store.
    if (std::none_of(pixels->begin(), pixels->end(),
                     [](std::uint32_t pixel) { return pixel != 0; })) {
        last_scanout_pixels_ = *pixels;
        return false;
    }

    last_scanout_pixels_ = *pixels;
    display_->replace_pixels(*pixels, owner_process_id);
    display_->present(owner_process_id);
    return true;
}

CoreSurfaceHle::Buffer* CoreSurfaceHle::find(std::uint32_t client) {
    const auto found = buffers_.find(client);
    return found == buffers_.end() ? nullptr : &found->second;
}

void CoreSurfaceHle::create_from_dictionary(UserlandHleCall& call,
                                            std::uint32_t dictionary) {
    if (dictionary == 0) {
        call.set_return(create_default_buffer(call));
        return;
    }
    auto request = std::make_shared<CreateRequest>();
    request->dictionary = dictionary;
    request->number_output =
        call.allocate_data(sizeof(std::uint32_t), alignof(std::uint32_t));
    if (request->number_output == 0) {
        call.set_return(0);
        return;
    }
    read_next_create_property(call, request);
}

void CoreSurfaceHle::read_next_create_property(
    UserlandHleCall& call, const std::shared_ptr<CreateRequest>& request) {
    if (request->property_index >= create_property_symbols.size()) {
        finish_create_from_dictionary(call, request);
        return;
    }
    const auto variable =
        call.symbol_address(create_property_symbols[request->property_index]);
    const auto key = variable ? call.memory().read32(*variable).value_or(0) : 0;
    if (key == 0) {
        ++request->property_index;
        read_next_create_property(call, request);
        return;
    }
    call.cpu().registers()[0] = request->dictionary;
    call.cpu().registers()[1] = key;
    if (!call.call_guest_function(
            "_CFDictionaryGetValue",
            [this, request](UserlandHleCall& value_call) {
                const auto value = value_call.cpu().registers()[0];
                if (value == 0) {
                    ++request->property_index;
                    read_next_create_property(value_call, request);
                    return;
                }
                if (!value_call.memory().write32(request->number_output, 0)) {
                    value_call.set_return(0);
                    return;
                }
                value_call.cpu().registers()[0] = value;
                value_call.cpu().registers()[1] = cf_number_sint32_type;
                value_call.cpu().registers()[2] = request->number_output;
                if (!value_call.call_guest_function(
                        "_CFNumberGetValue",
                        [this, request](UserlandHleCall& number_call) {
                            if (number_call.cpu().registers()[0] != 0) {
                                request->properties[request->property_index] =
                                    number_call.memory()
                                        .read32(request->number_output)
                                        .value_or(0);
                            }
                            ++request->property_index;
                            read_next_create_property(number_call, request);
                        })) {
                    value_call.set_return(0);
                }
            })) {
        call.set_return(0);
    }
}

void CoreSurfaceHle::finish_create_from_dictionary(
    UserlandHleCall& call, const std::shared_ptr<CreateRequest>& request) {
    const auto address = request->properties[client_address];
    auto size = request->properties[allocation_size];
    const auto surface_width = request->properties[width];
    const auto surface_height = request->properties[height];
    auto bytes_per_row = request->properties[pitch];
    auto format = request->properties[pixel_format];
    const auto offset = request->properties[data_offset];

    if (format == 0)
        format = surface_pixel_format_bgra;
    const auto bytes_per_pixel = surface_bytes_per_pixel(format);
    const auto row_bytes =
        static_cast<std::uint64_t>(surface_width) * bytes_per_pixel;
    if (bytes_per_row == 0 &&
        row_bytes <= std::numeric_limits<std::uint32_t>::max()) {
        bytes_per_row = static_cast<std::uint32_t>(row_bytes);
    }
    const auto required =
        surface_height == 0
            ? 0
            : static_cast<std::uint64_t>(surface_height - 1U) * bytes_per_row +
                  row_bytes;
    if (size == 0 && required <= std::numeric_limits<std::uint32_t>::max()) {
        size = static_cast<std::uint32_t>(required);
    }
    const auto valid_offset = offset <= size;
    const auto usable_size = valid_offset ? size - offset : 0;
    const auto valid_address =
        address <= std::numeric_limits<std::uint32_t>::max() - offset;
    const auto base = valid_address ? address + offset : 0;
    if (surface_width == 0 || surface_height == 0 || bytes_per_pixel == 0 ||
        row_bytes > bytes_per_row || required == 0 || required > usable_size ||
        usable_size > maximum_surface_bytes) {
        call.output().write("[coresurface-hle] invalid create properties\n");
        call.set_return(0);
        return;
    }

    auto owns_memory = address == 0;
    auto storage = base;
    if (owns_memory) {
        storage = call.allocate_data(usable_size, pixel_buffer_alignment);
    } else if (!call.memory().mapped(storage, usable_size)) {
        storage = 0;
    }
    call.set_return(storage == 0
                        ? 0
                        : create_buffer(call, storage, usable_size,
                                        surface_width, surface_height,
                                        bytes_per_row, format, owns_memory));
}

std::uint32_t
CoreSurfaceHle::create_default_buffer(UserlandHleCall& call,
                                      std::uint32_t requested_id) {
    const auto geometry =
        display_ ? display_->geometry() : default_display_geometry;
    const auto width = geometry.width;
    const auto height = geometry.height;
    const auto pitch = width * core_surface_abi::bytes_per_bgra_pixel;
    const auto size = pitch * height;
    const auto base = call.allocate_data(size, pixel_buffer_alignment);
    if (base == 0)
        return 0;
    return create_buffer(call, base, size, width, height, pitch,
                         surface_pixel_format_bgra, true, requested_id);
}

std::uint32_t CoreSurfaceHle::wrap_client_memory(UserlandHleCall& call,
                                                 std::uint32_t base,
                                                 std::uint32_t size) {
    if (base == 0 || size < core_surface_abi::bytes_per_bgra_pixel ||
        !call.memory().mapped(base, size)) {
        return 0;
    }
    const auto geometry =
        display_ ? display_->geometry() : default_display_geometry;
    const auto full_screen_size = geometry.width * geometry.height *
                                  core_surface_abi::bytes_per_bgra_pixel;
    const auto width =
        size >= full_screen_size
            ? geometry.width
            : std::max(1U, size / core_surface_abi::bytes_per_bgra_pixel);
    const auto height = size >= full_screen_size ? geometry.height : 1U;
    return create_buffer(call, base, size, width, height,
                         width * core_surface_abi::bytes_per_bgra_pixel,
                         surface_pixel_format_bgra, false);
}

std::uint32_t
CoreSurfaceHle::create_buffer(UserlandHleCall& call, std::uint32_t base,
                              std::uint32_t size, std::uint32_t width,
                              std::uint32_t height, std::uint32_t bytes_per_row,
                              std::uint32_t pixel_format, bool owns_memory,
                              std::uint32_t requested_id, bool publish) {
    const auto client = acquire_client_buffer(call);
    if (client == 0)
        return 0;
    const auto id =
        requested_id == 0 ? surfaces_->allocate_identifier() : requested_id;
    const std::pair<std::uint32_t, std::uint32_t> fields[]{
        {core_surface_abi::client_reference_count_offset, 1},
        {core_surface_abi::client_identifier_offset, id},
        {core_surface_abi::client_base_address_offset, base},
        {core_surface_abi::client_allocation_size_offset, size},
        {core_surface_abi::client_width_offset, width},
        {core_surface_abi::client_height_offset, height},
        {core_surface_abi::client_pitch_offset, bytes_per_row},
        {core_surface_abi::client_data_offset_offset, 0},
        {core_surface_abi::client_pixel_format_offset, pixel_format},
        {core_surface_abi::client_plane_count_offset, 0},
    };
    for (const auto& [offset, value] : fields) {
        if (!call.memory().write32(client + offset, value)) {
            recycle_client_buffer(client);
            return 0;
        }
    }
    auto backing = SurfaceStore::Backing{
        id, base, size, width, height, bytes_per_row, pixel_format, {}};
    backing.provenance.producer_process_id = call.process_id();
    if (pixel_format == surface_pixel_format_bgra &&
        !call.memory().track_write_generation(base, size)) {
        recycle_client_buffer(client);
        return 0;
    }
    if (publish && !surfaces_->publish(call.memory(), backing)) {
        recycle_client_buffer(client);
        return 0;
    }
    const auto application_viewport_minimum_height =
        display_ ? display_->height() -
                       std::min(display_->height(), std::uint32_t{64})
                 : 0U;
    if (publish && shared_state_ && display_ &&
        width == display_->width() &&
        height >= application_viewport_minimum_height &&
        height <= display_->height() &&
        bytes_per_row ==
            display_->width() * core_surface_abi::bytes_per_bgra_pixel) {
        const auto published = surfaces_->find(id);
        if (published && published->provenance.publication_sequence != 0U) {
            std::lock_guard lock{shared_state_->mach_mutex};
            const auto process = shared_state_->processes.find(
                call.process_id());
            if (process != shared_state_->processes.end() &&
                process->second.executable_path.starts_with(
                    "/Applications/")) {
                const auto publication_sequence =
                    published->provenance.publication_sequence;
                shared_state_
                    ->application_fullscreen_surface_publications
                        [call.process_id()]
                    .insert(publication_sequence);
                if (shared_state_
                        ->suppress_future_application_fullscreen_surface_processes
                        .contains(call.process_id())) {
                    shared_state_
                        ->suppressed_application_fullscreen_surface_publications
                        .emplace(call.process_id(), publication_sequence);
                    shared_state_
                        ->application_fullscreen_surface_suppression_active
                        .store(true, std::memory_order_release);
                }
            }
        }
    }
    buffers_[client] = Buffer{client,
                              id,
                              base,
                              size,
                              width,
                              height,
                              bytes_per_row,
                              pixel_format,
                              1,
                              owns_memory,
                              0,
                              0,
                              0,
                              {}};
    clients_by_id_[id] = client;
    call.output().write(
        "[coresurface-hle] create pid=" + std::to_string(call.process_id()) +
        " id=" + std::to_string(id) + " client=" + std::to_string(client) +
        " base=" + std::to_string(base) + " size=" + std::to_string(size) +
        "\n");
    return client;
}

std::uint32_t
CoreSurfaceHle::acquire_client_buffer(UserlandHleCall& call) {
    constexpr auto permissions =
        MemoryPermission::Read | MemoryPermission::Write;
    while (!free_client_buffers_.empty()) {
        const auto client = free_client_buffers_.back();
        free_client_buffers_.pop_back();
        if (!call.memory().accessible(
                client, core_surface_abi::client_buffer_structure_size,
                permissions)) {
            continue;
        }
        const std::array<std::byte,
                         core_surface_abi::client_buffer_structure_size>
            cleared{};
        if (call.memory().copy_in(client, cleared))
            return client;
    }

    const auto client =
        call.allocate_data(core_surface_abi::client_buffer_structure_size,
                           client_buffer_alignment);
    if (client == 0)
        return 0;
    const std::array<std::byte, core_surface_abi::client_buffer_structure_size>
        cleared{};
    return call.memory().copy_in(client, cleared) ? client : 0;
}

void CoreSurfaceHle::recycle_client_buffer(std::uint32_t client) {
    if (client != 0)
        free_client_buffers_.push_back(client);
}

std::uint32_t
CoreSurfaceHle::acquire_imported_mapping(UserlandHleCall& call,
                                         std::uint32_t size) {
    if (size == 0 || size % AddressSpace::page_size != 0)
        return 0;

    auto selected = free_imported_mappings_.end();
    for (auto candidate = free_imported_mappings_.begin();
         candidate != free_imported_mappings_.end();) {
        bool occupied = false;
        for (std::uint64_t page = candidate->first;
             page < static_cast<std::uint64_t>(candidate->first) +
                        candidate->second;
             page += AddressSpace::page_size) {
            if (call.memory().mapped(static_cast<std::uint32_t>(page))) {
                occupied = true;
                break;
            }
        }
        if (occupied) {
            candidate = free_imported_mappings_.erase(candidate);
            continue;
        }
        if (candidate->second < size) {
            ++candidate;
            continue;
        }
        if (selected == free_imported_mappings_.end() ||
            candidate->second < selected->second) {
            selected = candidate;
        }
        ++candidate;
    }
    if (selected != free_imported_mappings_.end()) {
        const auto address = selected->first;
        const auto available = selected->second;
        free_imported_mappings_.erase(selected);
        if (available > size) {
            free_imported_mappings_.emplace(address + size, available - size);
        }
        return address;
    }

    const auto address =
        call.allocate_data(size, AddressSpace::page_size);
    if (address == 0 || !call.memory().unmap(address, size))
        return 0;
    return address;
}

void CoreSurfaceHle::recycle_imported_mapping(std::uint32_t base,
                                              std::uint32_t size) {
    if (base == 0 || size == 0 ||
        base % AddressSpace::page_size != 0 ||
        size % AddressSpace::page_size != 0 ||
        size > std::numeric_limits<std::uint32_t>::max() - base) {
        return;
    }

    auto merged_base = base;
    auto merged_end = static_cast<std::uint64_t>(base) + size;
    auto next = free_imported_mappings_.lower_bound(base);
    if (next != free_imported_mappings_.begin()) {
        const auto previous = std::prev(next);
        const auto previous_end =
            static_cast<std::uint64_t>(previous->first) + previous->second;
        if (previous_end >= merged_base) {
            merged_base = previous->first;
            merged_end = std::max(merged_end, previous_end);
            free_imported_mappings_.erase(previous);
        }
    }
    next = free_imported_mappings_.lower_bound(merged_base);
    while (next != free_imported_mappings_.end() &&
           next->first <= merged_end) {
        merged_end =
            std::max(merged_end, static_cast<std::uint64_t>(next->first) +
                                     next->second);
        next = free_imported_mappings_.erase(next);
    }
    free_imported_mappings_.emplace(
        merged_base, static_cast<std::uint32_t>(merged_end - merged_base));
}

void CoreSurfaceHle::release_imported_mapping(AddressSpace& memory,
                                              std::uint32_t base,
                                              std::uint32_t size,
                                              std::uint64_t
                                                  mapping_lease_token) {
    if (base != 0 && size != 0 &&
        memory.unmap_mapping_lease(mapping_lease_token)) {
        recycle_imported_mapping(base, size);
    }
}

void CoreSurfaceHle::submit(Buffer& buffer, UserlandHleCall& call) {
    if (!display_ || buffer.width != display_->width() ||
        buffer.height != display_->height() ||
        buffer.bytes_per_row !=
            display_->width() * core_surface_abi::bytes_per_bgra_pixel) {
        return;
    }
    if (shared_state_) {
        std::lock_guard lock{shared_state_->mach_mutex};
        const auto process = shared_state_->processes.find(call.process_id());
        if (process != shared_state_->processes.end() &&
            process->second.executable_path.starts_with("/Applications/") &&
            !active_application_owns_display_locked(
                *shared_state_, call.process_id(),
                scene_coordinator_
                    ? std::optional<bool>{scene_coordinator_
                                              ->client_scene_presentable(
                                                  call.process_id())}
                    : std::nullopt)) {
            return;
        }
    }
    // Unlock publishes guest writes to the CoreSurface backing. Once the
    // firmware has entered transactional hardware-layer presentation, only
    // IOMobileFramebuffer SwapEnd may turn those backing updates into scanout.
    if (presentation_tracker_ && presentation_tracker_->has_presented_frame()) {
        return;
    }
    const auto bytes =
        call.memory().read_bytes(buffer.base, buffer.allocation_size);
    if (!bytes || bytes->size() < static_cast<std::size_t>(display_->width()) *
                                      display_->height() *
                                      core_surface_abi::bytes_per_bgra_pixel) {
        return;
    }
    std::vector<std::uint32_t> pixels(display_->geometry().pixel_count());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto offset = index * core_surface_abi::bytes_per_bgra_pixel;
        pixels[index] =
            std::to_integer<std::uint32_t>((*bytes)[offset]) |
            (std::to_integer<std::uint32_t>((*bytes)[offset + 1U]) << 8U) |
            (std::to_integer<std::uint32_t>((*bytes)[offset + 2U]) << 16U) |
            (std::to_integer<std::uint32_t>((*bytes)[offset + 3U]) << 24U);
    }
    display_->replace_pixels(std::move(pixels), call.process_id());
}

void CoreSurfaceHle::dispatch(UserlandHleCall& call) {
    const auto symbol = call.symbol();
    if (symbol == client_buffer_wrap_image_transport) {
        const auto width = call.argument(0);
        const auto height = call.argument(1);
        const auto pixel_format = call.argument(2);
        const auto bytes_per_row = call.argument(3);
        const auto allocation_size = call.argument(4);
        const auto base = call.argument(5);
        const auto output = call.argument(6);
        const auto bytes_per_pixel = surface_bytes_per_pixel(pixel_format);
        const auto row_bytes =
            static_cast<std::uint64_t>(width) * bytes_per_pixel;
        const auto required =
            height == 0
                ? 0
                : static_cast<std::uint64_t>(height - 1U) * bytes_per_row +
                      row_bytes;
        if (output == 0 || !call.memory().mapped(output, sizeof(std::uint32_t)) ||
            base == 0 || width == 0 || height == 0 ||
            bytes_per_pixel == 0 || row_bytes > bytes_per_row ||
            required == 0 || required > allocation_size ||
            allocation_size > maximum_surface_bytes ||
            !call.memory().mapped(base, allocation_size)) {
            call.set_return(1);
            return;
        }
        const auto client =
            create_buffer(call, base, allocation_size, width, height,
                          bytes_per_row, pixel_format, false);
        const auto* buffer = find(client);
        // The firmware's CoreSurfaceClientBufferAlloc owns the public wrapper
        // and expects the transport transaction to return its numeric object
        // identifier. Keep the HLE client private to the transport layer.
        if (buffer == nullptr ||
            !call.memory().write32(output, buffer->id)) {
            call.set_return(1);
            return;
        }
        call.set_return(success);
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferCreate") {
        create_from_dictionary(call, call.argument(0));
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferWrapClientMemory") {
        call.set_return(
            wrap_client_memory(call, call.argument(0), call.argument(1)));
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferLookup") {
        const auto requested_id = call.argument(0);
        if (requested_id == 0) {
            call.set_return(0);
            return;
        }
        auto client = clients_by_id_.find(requested_id);
        const auto retain_existing = client != clients_by_id_.end();
        if (client == clients_by_id_.end()) {
            std::uint32_t created = 0;
            if (const auto local = surfaces_->find(requested_id)) {
                created = create_buffer(
                    call, local->base, local->allocation_size, local->width,
                    local->height, local->bytes_per_row, local->pixel_format,
                    false, requested_id, false);
            } else if (const auto shared =
                           surfaces_->shared_mapping(requested_id)) {
                const auto mapping_address =
                    acquire_imported_mapping(call, shared->mapping_size);
                if (mapping_address != 0) {
                    std::uint64_t mapping_lease_token = 0;
                    if (const auto imported = surfaces_->import(
                            call.memory(), *shared, mapping_address,
                            &mapping_lease_token)) {
                        created = create_buffer(
                            call, imported->base, imported->allocation_size,
                            imported->width, imported->height,
                            imported->bytes_per_row, imported->pixel_format,
                            false, requested_id, false);
                        if (auto* buffer = find(created)) {
                            buffer->imported_mapping_base = mapping_address;
                            buffer->imported_mapping_size =
                                shared->mapping_size;
                            buffer->imported_mapping_lease_token =
                                mapping_lease_token;
                        } else {
                            surfaces_->release(requested_id);
                            release_imported_mapping(
                                call.memory(), mapping_address,
                                shared->mapping_size,
                                mapping_lease_token);
                        }
                    } else {
                        // import() installs the complete page range or nothing.
                        // Do not unmap on failure: the guest may have occupied
                        // this candidate after the availability check. The
                        // next acquisition revalidates every recycled page.
                        recycle_imported_mapping(mapping_address,
                                                 shared->mapping_size);
                    }
                }
            } else {
                created = create_default_buffer(call, requested_id);
            }
            if (created == 0) {
                call.set_return(0);
                return;
            }
            client = clients_by_id_.find(requested_id);
        }
        auto* buffer = find(client->second);
        if (buffer && retain_existing) {
            ++buffer->references;
            static_cast<void>(call.memory().write32(
                buffer->client +
                    core_surface_abi::client_reference_count_offset,
                buffer->references));
        }
        call.set_return(client->second);
        return;
    }

    const auto argument = call.argument(0);
    auto* buffer = find(argument);
    if (buffer == nullptr) {
        // Firmware-created CoreSurfaceClientBuffer objects store the transport
        // identifier at offset 4. Also accept a direct transport identifier
        // for callers that cross the private ABI without a public wrapper.
        auto client = clients_by_id_.find(argument);
        if (client == clients_by_id_.end() &&
            argument <= std::numeric_limits<std::uint32_t>::max() -
                            core_surface_abi::client_identifier_offset) {
            if (const auto identifier = call.memory().read32(
                    argument +
                    core_surface_abi::client_identifier_offset)) {
                client = clients_by_id_.find(*identifier);
            }
        }
        if (client != clients_by_id_.end()) {
            buffer = find(client->second);
        }
    }
    if (buffer == nullptr) {
        call.set_return(0);
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferRetain") {
        ++buffer->references;
        static_cast<void>(call.memory().write32(
            buffer->client + core_surface_abi::client_reference_count_offset,
            buffer->references));
        call.set_return(argument);
    } else if (symbol == "_CoreSurfaceClientBufferRelease") {
        if (buffer->references != 0)
            --buffer->references;
        static_cast<void>(call.memory().write32(
            buffer->client + core_surface_abi::client_reference_count_offset,
            buffer->references));
        if (buffer->references == 0) {
            const auto client = buffer->client;
            const auto id = buffer->id;
            const auto imported_mapping_base =
                buffer->imported_mapping_base;
            const auto imported_mapping_size =
                buffer->imported_mapping_size;
            const auto imported_mapping_lease_token =
                buffer->imported_mapping_lease_token;
            const auto indexed = clients_by_id_.find(id);
            if (indexed != clients_by_id_.end() &&
                indexed->second == client) {
                clients_by_id_.erase(indexed);
            }
            buffers_.erase(client);
            surfaces_->release(id);
            release_imported_mapping(call.memory(), imported_mapping_base,
                                     imported_mapping_size,
                                     imported_mapping_lease_token);
            recycle_client_buffer(client);
        }
        call.set_return(0);
    } else if (symbol == "_CoreSurfaceClientBufferGetID") {
        call.set_return(buffer->id);
    } else if (symbol == "_CoreSurfaceClientBufferGetAllocSize") {
        call.set_return(buffer->allocation_size);
    } else if (symbol == "_CoreSurfaceClientBufferGetWidth") {
        call.set_return(buffer->width);
    } else if (symbol == "_CoreSurfaceClientBufferGetWidthOfPlane") {
        call.set_return(call.argument(1) == 0 ? buffer->width : 0);
    } else if (symbol == "_CoreSurfaceClientBufferGetHeight") {
        call.set_return(buffer->height);
    } else if (symbol == "_CoreSurfaceClientBufferGetHeightOfPlane") {
        call.set_return(call.argument(1) == 0 ? buffer->height : 0);
    } else if (symbol == "_CoreSurfaceClientBufferGetBytesPerRow") {
        call.set_return(buffer->bytes_per_row);
    } else if (symbol == "_CoreSurfaceClientBufferGetBytesPerRowOfPlane") {
        call.set_return(call.argument(1) == 0 ? buffer->bytes_per_row : 0);
    } else if (symbol == "_CoreSurfaceClientBufferGetPixelFormatType") {
        call.set_return(buffer->pixel_format);
    } else if (symbol == "_CoreSurfaceClientBufferGetBaseAddress") {
        call.set_return(buffer->base);
    } else if (symbol == "_CoreSurfaceClientBufferGetBaseAddressOfPlane") {
        call.set_return(call.argument(1) == 0 ? buffer->base : 0);
    } else if (symbol == "_CoreSurfaceClientBufferGetPlaneCount") {
        call.set_return(0);
    } else if (symbol == "_CoreSurfaceClientBufferLock") {
        const auto options = call.argument(1);
        const auto synchronized = surfaces_->synchronize_for_cpu(
            call.memory(), buffer->id,
            SurfaceStore::CpuSynchronizationOptions{
                .avoid_sync =
                    (options & core_surface_abi::lock_avoid_sync) != 0,
                .read_only =
                    (options & core_surface_abi::lock_read_only) != 0,
            });
        if (synchronized)
            buffer->lock_options.push_back(options);
        call.set_return(synchronized ? success : 1U);
    } else if (symbol ==
               "_CoreSurfaceClientBufferFlushProcessorCaches") {
        static_cast<void>(
            surfaces_->synchronize_from_guest(call.memory(), buffer->id));
        call.set_return(success);
    } else if (symbol == "_CoreSurfaceClientBufferUnlock") {
        // A matching explicit Lock is authoritative. WrapClientImage also
        // owns an implicit lock and later calls Unlock with its options in r1,
        // so preserve that firmware argument when no explicit Lock was seen.
        auto options = call.argument(1);
        if (!buffer->lock_options.empty()) {
            options = buffer->lock_options.back();
            buffer->lock_options.pop_back();
        }
        if ((options & core_surface_abi::lock_read_only) == 0) {
            static_cast<void>(surfaces_->synchronize_from_guest(
                call.memory(), buffer->id));
            submit(*buffer, call);
        }
        call.set_return(success);
    } else if (symbol == "_CoreSurfaceClientBufferGetYCbCrMatrix" ||
               symbol == "_CoreSurfaceClientBufferCopyProperty") {
        call.set_return(0);
    } else if (symbol == "_CoreSurfaceClientBufferSetYCbCrMatrix" ||
               symbol == "_CoreSurfaceClientBufferSetProperty" ||
               symbol == "_CoreSurfaceClientBufferRemoveProperty") {
        call.set_return(success);
    } else {
        if (unsupported_trace_count_ < maximum_unsupported_traces) {
            call.output().write(
                "[coresurface-hle] deferred symbol=" + std::string{symbol} +
                " pid=" + std::to_string(call.process_id()) + "\n");
            ++unsupported_trace_count_;
        }
        call.set_return(0);
    }
}

} // namespace ilemu
