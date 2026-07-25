#include "ilegacysim/core_surface_hle.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ilegacysim/address_space.hpp"
#include "ilegacysim/core_surface_abi.hpp"
#include "ilegacysim/display.hpp"
#include "ilegacysim/iokit_abi.hpp"
#include "ilegacysim/kernel_shared_state.hpp"
#include "ilegacysim/output.hpp"
#include "ilegacysim/presentation_tracker.hpp"
#include "ilegacysim/scene_coordinator.hpp"
#include "ilegacysim/surface_store.hpp"
#include "ilegacysim/userland_hle.hpp"

namespace ilegacysim {
namespace {

constexpr std::string_view core_surface_image{
    "/System/Library/Frameworks/CoreSurface.framework/CoreSurface"};
constexpr std::string_view client_buffer_prefix{"_CoreSurfaceClientBuffer"};

constexpr std::size_t client_buffer_alignment = 16;
constexpr std::size_t pixel_buffer_alignment = 64;
constexpr std::size_t maximum_unsupported_traces = 32;

// Bytes in guest memory are B,G,R,A; when read as a little-endian uint32_t
// this is the DisplayState 0xAARRGGBB representation without conversion.
constexpr std::uint32_t success = 0;

}  // namespace

CoreSurfaceHle::CoreSurfaceHle(
    UserlandHleRegistry& registry, std::shared_ptr<DisplayState> display,
    std::shared_ptr<SurfaceStore> surfaces,
    std::shared_ptr<PresentationTracker> presentations)
    : display_{std::move(display)},
      surfaces_{surfaces ? std::move(surfaces)
                         : std::make_shared<SurfaceStore>()},
      presentation_tracker_{std::move(presentations)} {
    registry.register_prefix(
        std::string{core_surface_image}, std::string{client_buffer_prefix},
        [this](UserlandHleCall& call) { dispatch(call); });
}

void CoreSurfaceHle::reset() {
    buffers_.clear();
    clients_by_id_.clear();
    unsupported_trace_count_ = 0;
    last_scanout_generation_.reset();
    last_scanout_pixels_.clear();
    surfaces_->reset();
}

void CoreSurfaceHle::inherit_state(const CoreSurfaceHle& parent) {
    buffers_ = parent.buffers_;
    clients_by_id_ = parent.clients_by_id_;
    last_scanout_generation_ = parent.last_scanout_generation_;
    last_scanout_pixels_ = parent.last_scanout_pixels_;
    presentation_tracker_ = parent.presentation_tracker_;
    surfaces_->inherit_state(*parent.surfaces_);
}

void CoreSurfaceHle::set_display(std::shared_ptr<DisplayState> display) {
    display_ = std::move(display);
}

void CoreSurfaceHle::set_shared_state(
    std::shared_ptr<KernelSharedState> shared_state) {
    shared_state_ = std::move(shared_state);
}

void CoreSurfaceHle::set_scene_coordinator(
    std::shared_ptr<SceneCoordinator> scenes) {
    scene_coordinator_ = std::move(scenes);
}

bool CoreSurfaceHle::refresh_default_scanout(AddressSpace& memory) {
    if (!display_) return false;
    const auto backing = surfaces_->find(
        iokit_abi::apple_h1clcd_default_surface_id);
    if (!backing) return false;
    const auto generation = memory.range_write_generation(
        backing->base, backing->allocation_size);
    if (!generation || generation == last_scanout_generation_) return false;
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
    display_->replace_pixels(*pixels);
    display_->present();
    return true;
}

CoreSurfaceHle::Buffer* CoreSurfaceHle::find(std::uint32_t client) {
    const auto found = buffers_.find(client);
    return found == buffers_.end() ? nullptr : &found->second;
}

std::uint32_t CoreSurfaceHle::create_default_buffer(
    UserlandHleCall& call, std::uint32_t requested_id) {
    const auto geometry = display_ ? display_->geometry()
                                   : default_display_geometry;
    const auto width = geometry.width;
    const auto height = geometry.height;
    const auto pitch = width * core_surface_abi::bytes_per_bgra_pixel;
    const auto size = pitch * height;
    const auto base = call.allocate_data(size, pixel_buffer_alignment);
    if (base == 0) return 0;
    return create_buffer(
        call, base, size, width, height, true, requested_id);
}

std::uint32_t CoreSurfaceHle::wrap_client_memory(
    UserlandHleCall& call, std::uint32_t base, std::uint32_t size) {
    if (base == 0 || size < core_surface_abi::bytes_per_bgra_pixel ||
        !call.memory().mapped(base, size)) {
        return 0;
    }
    const auto geometry = display_ ? display_->geometry()
                                   : default_display_geometry;
    const auto full_screen_size = geometry.width *
                                  geometry.height *
                                  core_surface_abi::bytes_per_bgra_pixel;
    const auto width = size >= full_screen_size
                           ? geometry.width
                           : std::max(
                                 1U, size /
                                         core_surface_abi::bytes_per_bgra_pixel);
    const auto height = size >= full_screen_size ? geometry.height : 1U;
    return create_buffer(call, base, size, width, height, false);
}

std::uint32_t CoreSurfaceHle::create_buffer(
    UserlandHleCall& call, std::uint32_t base, std::uint32_t size,
    std::uint32_t width, std::uint32_t height, bool owns_memory,
    std::uint32_t requested_id, bool publish) {
    const auto client = call.allocate_data(
        core_surface_abi::client_buffer_structure_size,
        client_buffer_alignment);
    if (client == 0) return 0;
    const auto id = requested_id == 0
                        ? surfaces_->allocate_identifier()
                        : requested_id;
    const auto pitch = width * core_surface_abi::bytes_per_bgra_pixel;

    const std::pair<std::uint32_t, std::uint32_t> fields[]{
        {core_surface_abi::client_reference_count_offset, 1},
        {core_surface_abi::client_identifier_offset, id},
        {core_surface_abi::client_base_address_offset, base},
        {core_surface_abi::client_allocation_size_offset, size},
        {core_surface_abi::client_width_offset, width},
        {core_surface_abi::client_height_offset, height},
        {core_surface_abi::client_pitch_offset, pitch},
        {core_surface_abi::client_data_offset_offset, 0},
        {core_surface_abi::client_pixel_format_offset,
         surface_pixel_format_bgra},
        {core_surface_abi::client_plane_count_offset, 0},
    };
    for (const auto& [offset, value] : fields) {
        if (!call.memory().write32(client + offset, value)) return 0;
    }
    auto backing = SurfaceStore::Backing{
        id, base, size, width, height, pitch, surface_pixel_format_bgra, {}};
    backing.provenance.producer_process_id = call.process_id();
    if (id == iokit_abi::apple_h1clcd_default_surface_id &&
        !call.memory().track_write_generation(base, size)) {
        return 0;
    }
    if (publish && !surfaces_->publish(call.memory(), backing)) return 0;
    std::vector<std::uint32_t> preserved_exit_snapshot_pixels;
    const auto geometry = display_ ? display_->geometry()
                                   : default_display_geometry;
    if (shared_state_ && publish && owns_memory &&
        width == geometry.width &&
        height == geometry.height &&
        pitch == geometry.width *
                     core_surface_abi::bytes_per_bgra_pixel) {
        std::lock_guard lock{shared_state_->mach_mutex};
        auto& pending = shared_state_->pending_application_exit_snapshot;
        if (pending && pending->process_id == call.process_id() &&
            pending->pixels.size() ==
                static_cast<std::size_t>(width) * height) {
            preserved_exit_snapshot_pixels = std::move(pending->pixels);
            pending.reset();
        }
    }
    buffers_[client] = Buffer{
        client, id, base, size, width, height, pitch,
        surface_pixel_format_bgra, 1, owns_memory,
        std::move(preserved_exit_snapshot_pixels)};
    clients_by_id_[id] = client;
    call.output().write(
        "[coresurface-hle] create pid=" + std::to_string(call.process_id()) +
        " id=" + std::to_string(id) + " client=" +
        std::to_string(client) + " base=" + std::to_string(base) +
        " size=" + std::to_string(size) + "\n");
    return client;
}

void CoreSurfaceHle::submit(Buffer& buffer, UserlandHleCall& call) {
    if (!display_ || buffer.width != display_->width() ||
        buffer.height != display_->height() ||
        buffer.bytes_per_row != display_->width() *
                                    core_surface_abi::bytes_per_bgra_pixel) {
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
                    ? std::optional<bool>{
                          scene_coordinator_->client_scene_active(
                              call.process_id())}
                    : std::nullopt)) {
            return;
        }
    }
    // Unlock publishes guest writes to the CoreSurface backing. Once the
    // firmware has entered transactional hardware-layer presentation, only
    // IOMobileFramebuffer SwapEnd may turn those backing updates into scanout.
    if (presentation_tracker_ &&
        presentation_tracker_->has_presented_frame()) {
        return;
    }
    const auto bytes = call.memory().read_bytes(buffer.base, buffer.allocation_size);
    if (!bytes || bytes->size() < static_cast<std::size_t>(
                                    display_->width()) *
                                    display_->height() *
                                    core_surface_abi::bytes_per_bgra_pixel) {
        return;
    }
    std::vector<std::uint32_t> pixels(
        display_->geometry().pixel_count());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        const auto offset =
            index * core_surface_abi::bytes_per_bgra_pixel;
        pixels[index] = std::to_integer<std::uint32_t>((*bytes)[offset]) |
                        (std::to_integer<std::uint32_t>((*bytes)[offset + 1U]) << 8U) |
                        (std::to_integer<std::uint32_t>((*bytes)[offset + 2U]) << 16U) |
                        (std::to_integer<std::uint32_t>((*bytes)[offset + 3U]) << 24U);
    }
    display_->replace_pixels(std::move(pixels));
}

void CoreSurfaceHle::dispatch(UserlandHleCall& call) {
    const auto symbol = call.symbol();
    if (symbol == "_CoreSurfaceClientBufferCreate") {
        call.set_return(create_default_buffer(call));
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferAlloc") {
        const auto geometry = display_ ? display_->geometry()
                                       : default_display_geometry;
        const auto size = geometry.pixel_count() *
                          core_surface_abi::bytes_per_bgra_pixel;
        const auto base = call.allocate_data(size, pixel_buffer_alignment);
        call.set_return(base == 0 ? 0 : create_buffer(
            call, base, static_cast<std::uint32_t>(size), geometry.width,
            geometry.height, true, call.argument(0)));
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferWrapClientMemory") {
        call.set_return(wrap_client_memory(
            call, call.argument(0), call.argument(1)));
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferLookup") {
        const auto requested_id = call.argument(0);
        if (requested_id == 0) {
            call.set_return(0);
            return;
        }
        auto client = clients_by_id_.find(requested_id);
        if (client == clients_by_id_.end()) {
            std::uint32_t created = 0;
            if (const auto shared = surfaces_->shared_mapping(requested_id)) {
                const auto mapping_address = call.allocate_data(
                    shared->mapping_size, AddressSpace::page_size);
                if (mapping_address != 0 &&
                    call.memory().unmap(mapping_address,
                                        shared->mapping_size)) {
                    if (const auto imported = surfaces_->import(
                            call.memory(), requested_id, mapping_address)) {
                        created = create_buffer(
                            call, imported->base, imported->allocation_size,
                            imported->width, imported->height, false,
                            requested_id, false);
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
        if (buffer) ++buffer->references;
        call.set_return(client->second);
        return;
    }

    auto* buffer = find(call.argument(0));
    if (buffer == nullptr) {
        call.set_return(0);
        return;
    }
    if (symbol == "_CoreSurfaceClientBufferRetain") {
        ++buffer->references;
        static_cast<void>(call.memory().write32(
            buffer->client + core_surface_abi::client_reference_count_offset,
            buffer->references));
        call.set_return(buffer->client);
    } else if (symbol == "_CoreSurfaceClientBufferRelease") {
        if (buffer->references != 0) --buffer->references;
        static_cast<void>(call.memory().write32(
            buffer->client + core_surface_abi::client_reference_count_offset,
            buffer->references));
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
    } else if (symbol == "_CoreSurfaceClientBufferLock" ||
               symbol == "_CoreSurfaceClientBufferFlushProcessorCaches") {
        call.set_return(success);
    } else if (symbol == "_CoreSurfaceClientBufferUnlock") {
        if (!buffer->preserved_exit_snapshot_pixels.empty()) {
            std::vector<std::byte> bytes(
                buffer->preserved_exit_snapshot_pixels.size() *
                core_surface_abi::bytes_per_bgra_pixel);
            for (std::size_t index = 0;
                 index < buffer->preserved_exit_snapshot_pixels.size();
                 ++index) {
                const auto pixel =
                    buffer->preserved_exit_snapshot_pixels[index];
                const auto offset =
                    index * core_surface_abi::bytes_per_bgra_pixel;
                bytes[offset] = static_cast<std::byte>(pixel);
                bytes[offset + 1U] = static_cast<std::byte>(pixel >> 8U);
                bytes[offset + 2U] = static_cast<std::byte>(pixel >> 16U);
                bytes[offset + 3U] = static_cast<std::byte>(pixel >> 24U);
            }
            if (call.memory().copy_in(buffer->base, bytes)) {
                buffer->preserved_exit_snapshot_pixels.clear();
            }
        }
        submit(*buffer, call);
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

}  // namespace ilegacysim
