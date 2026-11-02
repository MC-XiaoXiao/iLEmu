// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "mbx_cpu_draw.hpp"
#include "graphics/gles_renderer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ilemu {
bool Mbx2dHle::CpuDraw::Framebuffer::execute(
    const std::shared_ptr<GlesRenderer>& renderer, DisplayFrame& frame)
{
    const auto width = source.width;
    const auto height = source.height;
    if (source.host_surface) {
        if (!renderer->read_surface(*source.host_surface, frame,
                PerfCpuMapReason::DeferredDisplayRead))
            return false;
        generation = std::max(source.host_surface->cpu_generation(),
            source.host_surface->gpu_generation());
    } else {
        frame = std::move(source);
    }
    if (frame.width != width || frame.height != height ||
        frame.pixels.size() != static_cast<std::size_t>(width) * height)
        return false;
    if (!output)
        return true; // Source capture; no target merge.
    const auto r = output->rectangle;
    std::optional<std::vector<std::uint32_t>> merged;
    if (output->blend || output->coverage) {
        std::vector<std::uint32_t> destination(
            static_cast<std::size_t>(r.width) * r.height);
        for (std::uint32_t row = 0; row < r.height; ++row)
            std::copy_n(frame.pixels.begin() +
                            (static_cast<std::size_t>(r.y) + row) * width + r.x,
                r.width,
                destination.begin() + static_cast<std::size_t>(row) * r.width);
        merged = output->merge(std::move(destination));
        if (!merged)
            return false;
    }
    const auto& pixels = merged ? *merged : output->pixels;
    for (std::uint32_t row = 0; row < r.height; ++row)
        std::copy_n(pixels.begin() + static_cast<std::size_t>(row) * r.width,
            r.width,
            frame.pixels.begin() +
                (static_cast<std::size_t>(r.y) + row) * width + r.x);
    return true;
}

void Mbx2dHle::await_framebuffer(
    UserlandHleCall& call, std::shared_ptr<CpuDraw> cpu, bool write)
try {
    if (!cpu_draw_live(call, *cpu, !write))
        throw std::runtime_error { "stale MBX framebuffer binding" };
    auto& paint = *cpu->output;
    const auto r = paint.rectangle;
    if (write &&
        paint.pixels.size() != static_cast<std::uint64_t>(r.width) * r.height)
        throw std::invalid_argument { "invalid MBX framebuffer pixels" };
    if (write && !paint.blend && !paint.coverage && r.x == 0 && r.y == 0 &&
        r.width == cpu->destination.width &&
        r.height == cpu->destination.height) {
        // Full replacement has no dependency on the previous scanout image.
        display_->replace_pixels(std::move(paint.pixels), call.process_id());
        cpu->lease->consume();
        call.set_return(mbx2d_abi::success);
        return;
    }
    if (!cpu->framebuffer) {
        auto frame = std::make_shared<CpuDraw::Framebuffer>();
        frame->credit = cpu->credit;
        if (write)
            frame->output = cpu->output;
        frame->revision = display_->content_revision();
        frame->source = display_->capture_frame();
        cpu->framebuffer = std::move(frame);
    }
    const auto framebuffer = cpu->framebuffer;
    const auto& surface = write ? cpu->destination : *cpu->source;
    const auto bytes = static_cast<std::size_t>(surface.width) *
                       surface.height * sizeof(std::uint32_t);
    if (!framebuffer->result) {
        const auto source = framebuffer->source.host_surface;
        framebuffer->result = enqueue_gles_host_work(bytes,
            [framebuffer, renderer = host_graphics_](DisplayFrame& frame) {
                return framebuffer->execute(renderer, frame);
            });
        if (!framebuffer->result) {
            call.suspend_for_host(
                [bytes] { return gles_host_work_capacity(bytes); },
                [this, cpu, write](UserlandHleCall& resumed) {
                    await_framebuffer(resumed, cpu, write);
                });
            return;
        }
        cpu->lease->result = framebuffer->result;
        if (source)
            source->record_read(framebuffer->result->completion());
    }
    const auto result = framebuffer->result;
    call.suspend_for_host([result] { return result->ready(); },
        [this, cpu, framebuffer, result, write](UserlandHleCall& resumed) {
            try {
                if (!cpu_draw_live(resumed, *cpu, !write) ||
                    !result->succeeded())
                    throw std::runtime_error {
                        "MBX framebuffer capture failed"
                    };
                if (!write) {
                    cpu->source->cpu_frame =
                        std::make_shared<DisplayFrame>(std::move(result->frame));
                    cpu->framebuffer.reset();
                    cpu->lease->result.reset();
                    await_cpu_draw(resumed, cpu);
                    return;
                }
                const auto source = framebuffer->source.host_surface;
                if (framebuffer->revision != display_->content_revision() ||
                    (source && framebuffer->generation !=
                                   std::max(source->cpu_generation(),
                                       source->gpu_generation()))) {
                    // Preserve newer pixels outside this operation's ROI. Only
                    // target read/merge retries; the original CPU output is
                    // frozen.
                    cpu->framebuffer.reset();
                    cpu->lease->result.reset();
                    await_framebuffer(resumed, cpu, true);
                    return;
                }
                display_->replace_pixels(
                    std::move(result->frame.pixels), resumed.process_id());
                cpu->lease->consume();
                resumed.set_return(mbx2d_abi::success);
            } catch (...) {
                cpu->lease->consume();
                resumed.set_return(mbx2d_abi::failure);
            }
        },
        result->sequence());
} catch (...) {
    cpu->lease->cancelled = true;
    if (cpu->lease->ready())
        cpu->lease->consume();
    call.set_return(mbx2d_abi::failure);
}
} // namespace ilemu
