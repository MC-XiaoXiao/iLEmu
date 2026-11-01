// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "graphics/surface_store.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    std::size_t frame_bytes(std::uint32_t width, std::uint32_t height)
    {
        constexpr std::size_t limit = 64U * 1024U * 1024U;
        const auto pixels = static_cast<std::uint64_t>(width) * height;
        if (pixels > limit / sizeof(std::uint32_t))
            throw std::length_error { "GLES readback exceeds result budget" };
        return static_cast<std::size_t>(pixels) * sizeof(std::uint32_t);
    }
}

// Core-only. The registry retains the scalar ABI; this record additionally
// tracks its original destination mapping and pixel-pack layout across every
// graphics dependency and queue-capacity wait. It owns no guest host pointer.
struct OpenGlesHle::ReadPixelsOutput {
    AddressSpace::MappingObservation mapping;
    std::size_t stride { }, extent { };
};

std::shared_ptr<OpenGlesHle::ReadPixelsOutput>
OpenGlesHle::capture_read_pixels_output(UserlandHleCall& call)
{
    auto output = std::make_shared<ReadPixelsOutput>();
    const auto* context = current_context(call);
    const auto width = static_cast<std::int32_t>(call.argument(2));
    const auto height = static_cast<std::int32_t>(call.argument(3));
    if (!context || width <= 0 || height <= 0 || !call.argument(6))
        return output; // Normal handler preserves input/no-op error ordering.
    const auto alignment = static_cast<std::uint64_t>(context->pack_alignment);
    if (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)
        return output;
    const auto row = static_cast<std::uint64_t>(width) * 4U;
    const auto stride = (row + alignment - 1U) & ~(alignment - 1U);
    const auto extent =
        stride * (static_cast<std::uint32_t>(height) - 1U) + row;
    if (extent > gles_abi::maximum_resource_bytes ||
        static_cast<std::uint64_t>(call.argument(6)) + extent >
            (std::uint64_t { 1 } << 32U))
        return output;
    output->stride = static_cast<std::size_t>(stride);
    output->extent = static_cast<std::size_t>(extent);
    output->mapping =
        call.memory().observe_mapping(call.argument(6), output->extent);
    return output;
}

void OpenGlesHle::read_pixels(
    UserlandHleCall& call, std::shared_ptr<ReadPixelsOutput> destination)
try {
    auto* context = current_context(call);
    if (!context) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto x = static_cast<std::int32_t>(call.argument(0));
    const auto y = static_cast<std::int32_t>(call.argument(1));
    const auto width = static_cast<std::int32_t>(call.argument(2));
    const auto height = static_cast<std::int32_t>(call.argument(3));
    if ((call.argument(4) != gles_abi::rgba &&
            call.argument(4) != gles_abi::bgra_apple) ||
        call.argument(5) != gles_abi::unsigned_byte) {
        set_gl_error(call, gles_abi::invalid_enum);
        return;
    }
    if (x < 0 || y < 0 || width < 0 || height < 0 ||
        (width && height && call.argument(6) == 0)) {
        set_gl_error(call, gles_abi::invalid_value);
        return;
    }
    if (!width || !height)
        return;
    if (!destination)
        destination = capture_read_pixels_output(call);
    if (!destination->mapping.valid()) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto binding = resolve_render_target(call, *context);
    if (!binding) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto display_source = binding->kind == RenderTargetKind::Display &&
                                !binding->display_surface &&
                                !binding->pixmap_surface;
    auto frame = display_source && display_
                     ? std::optional { display_->capture_frame() }
                     : render_target(call, *binding);
    if (!frame) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto surface =
        binding->host_surface ? binding->host_surface : frame->host_surface;
    std::size_t bytes;
    try {
        bytes = frame_bytes(frame->width, frame->height);
        if (surface) {
            const auto descriptor = surface->descriptor();
            bytes = std::max(
                bytes, frame_bytes(descriptor.width, descriptor.height));
        }
    } catch (...) {
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    }
    // Guest/reader callbacks never cross; retain the host content lease
    // separately from the plain CPU dimensions/pixels input.
    DisplayFrame snapshot { frame->width, frame->height, 0, { } };
    if (!surface)
        snapshot.pixels = std::move(frame->pixels);
    auto result = enqueue_gles_host_work(bytes,
        [renderer = renderer_, surface,
            source_lease = std::move(frame->presentation_lease),
            synchronize_target = !binding->host_surface, key = binding->key,
            frame = std::move(snapshot)](DisplayFrame& output) mutable {
            static_cast<void>(source_lease);
            if (surface) {
                if (!renderer->read_surface(*surface, output))
                    return false;
                // Preserve snapshot()+synchronize order without a core wait.
                return !synchronize_target ||
                       renderer->synchronize(output, key);
            }
            output = std::move(frame);
            return renderer->synchronize(output, key);
        });
    if (!result) {
        call.suspend_until([bytes] { return gles_host_work_capacity(bytes); },
            [this, destination](UserlandHleCall& resumed) {
                await_graphics_call(
                    resumed, [this, destination](UserlandHleCall& ready) {
                        read_pixels(ready, destination);
                    });
            });
        return;
    }
    if (surface)
        surface->record_read(result->completion());
    auto lease = std::make_shared<SyncLease>();
    lease->owner = call.cpu().processor_id();
    lease->result = result;
    sync_lease_ = lease;
    call.suspend_until([result] { return result->ready(); },
        [this, result, lease, inverted = binding->inverted_vertical,
            destination, backing = binding->backing_identifier,
            key = binding->key](UserlandHleCall& resumed) {
            try {
                const auto current =
                    backing ? surface_store_->find(*backing) : std::nullopt;
                const bool live =
                    !backing ||
                    (current && current->provenance.publication_sequence ==
                                    key.surface);
                if (!result->succeeded() || !live ||
                    !destination->mapping.valid())
                    set_gl_error(resumed, gles_abi::invalid_operation);
                else
                    write_read_pixels(
                        resumed, result->frame, inverted, *destination);
            } catch (const std::bad_alloc&) {
                set_gl_error(resumed, gles_abi::out_of_memory);
            } catch (...) {
                set_gl_error(resumed, gles_abi::invalid_operation);
            }
            lease->consume();
        },
        result->sequence());
} catch (const std::bad_alloc&) {
    // An accepted job retains its resources even if saving its observer fails.
    // Canceled observers are consumed at the normal core completion boundary.
    if (sync_lease_ && sync_lease_->owner == call.cpu().processor_id() &&
        !sync_lease_->consumed)
        sync_lease_->cancelled = true;
    set_gl_error(call, gles_abi::out_of_memory);
} catch (...) {
    if (sync_lease_ && sync_lease_->owner == call.cpu().processor_id() &&
        !sync_lease_->consumed)
        sync_lease_->cancelled = true;
    set_gl_error(call, gles_abi::invalid_operation);
}

void OpenGlesHle::write_read_pixels(UserlandHleCall& call,
    const DisplayFrame& frame, bool inverted,
    const ReadPixelsOutput& destination)
{
    const auto x = static_cast<std::int32_t>(call.argument(0));
    const auto y = static_cast<std::int32_t>(call.argument(1));
    const auto width = static_cast<std::int32_t>(call.argument(2));
    const auto height = static_cast<std::int32_t>(call.argument(3));
    const auto format = call.argument(4);
    const auto output = call.argument(6);
    if (frame.pixels.size() !=
        static_cast<std::size_t>(frame.width) * frame.height) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto unsigned_x = static_cast<std::uint32_t>(x);
    const auto unsigned_y = static_cast<std::uint32_t>(y);
    const auto unsigned_width = static_cast<std::uint32_t>(width);
    const auto unsigned_height = static_cast<std::uint32_t>(height);
    if (unsigned_x > frame.width || unsigned_y > frame.height ||
        unsigned_width > frame.width - unsigned_x ||
        unsigned_height > frame.height - unsigned_y) {
        set_gl_error(call, gles_abi::invalid_value);
        return;
    }

    constexpr std::uint64_t bytes_per_pixel = 4U;
    const auto stride = destination.stride;
    const auto total = destination.extent;
    if (!destination.mapping.valid() || !total ||
        !call.memory().accessible(output, total, MemoryPermission::Write)) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(total));
    for (std::uint32_t row = 0; row < unsigned_height; ++row) {
        const auto guest_y = unsigned_y + row;
        const auto host_y = inverted ? guest_y : frame.height - 1U - guest_y;
        for (std::uint32_t column = 0; column < unsigned_width; ++column) {
            const auto pixel =
                frame.pixels[static_cast<std::size_t>(host_y) * frame.width +
                             unsigned_x + column];
            const auto offset = static_cast<std::size_t>(
                static_cast<std::uint64_t>(row) * stride +
                static_cast<std::uint64_t>(column) * bytes_per_pixel);
            const auto red = static_cast<std::byte>((pixel >> 16U) & 0xffU);
            const auto green = static_cast<std::byte>((pixel >> 8U) & 0xffU);
            const auto blue = static_cast<std::byte>(pixel & 0xffU);
            bytes[offset] = format == gles_abi::rgba ? red : blue;
            bytes[offset + 1U] = green;
            bytes[offset + 2U] = format == gles_abi::rgba ? blue : red;
            bytes[offset + 3U] = static_cast<std::byte>(pixel >> 24U);
        }
    }
    const auto row_bytes = static_cast<std::size_t>(unsigned_width) * 4U;
    if (stride == row_bytes) {
        if (!call.memory().copy_to_user(output, bytes))
            set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    // Pack alignment skips padding; those bytes belong to the guest, including
    // writes made by other threads while this caller was suspended.
    for (std::uint32_t row = 0; row < unsigned_height; ++row) {
        const auto offset = static_cast<std::size_t>(row) * stride;
        if (!call.memory().copy_to_user(
                output + static_cast<std::uint32_t>(offset),
                std::span<const std::byte> { bytes }.subspan(
                    offset, row_bytes))) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
    }
}

} // namespace ilemu
