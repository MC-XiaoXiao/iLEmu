// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "session_snapshots.hpp"

#include "foundation/output.hpp"
#include "graphics/display.hpp"
#include "graphics/frame_file_presenter.hpp"
#include "graphics/gles_program_renderer.hpp"
#include "graphics/surface_cpu_readback.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    constexpr std::size_t pixel_limit = 64U * 1024U * 1024U;
    std::size_t frame_bytes(std::uint32_t width, std::uint32_t height)
    {
        const auto count = static_cast<std::uint64_t>(width) * height;
        if (!width || !height || count > pixel_limit / sizeof(std::uint32_t))
            throw std::length_error { "snapshot exceeds pixel budget" };
        return static_cast<std::size_t>(count) * sizeof(std::uint32_t);
    }
} // namespace

struct SessionSnapshots::Pending {
    // Release the preparation credit after retained CPU pixels.
    std::shared_ptr<void> credit;
    std::filesystem::path path;
    DisplayFrame source;
    bool sequence { };
    std::shared_ptr<GlesValueResult<SurfaceCpuReadback>> gpu;
};

SessionSnapshots::SessionSnapshots(
    std::shared_ptr<GlesRenderer> renderer, Output& output)
    : renderer_ { std::move(renderer) }
    , output_ { output }
{
}
SessionSnapshots::~SessionSnapshots() = default;

void SessionSnapshots::fail(
    const std::filesystem::path& path, const char* reason)
{
    output_.marker("[control] error: snapshot=" + path.string() + " " + reason);
}

void SessionSnapshots::publish(
    const Pending& request, const DisplayFrame& frame)
{
    if (frame.pixels.size() !=
        frame_bytes(frame.width, frame.height) / sizeof(std::uint32_t))
        throw std::runtime_error { "snapshot pixels are incomplete" };
    switch (frame.orientation) {
    case DisplayOrientation::Portrait:
    case DisplayOrientation::PortraitUpsideDown:
    case DisplayOrientation::LandscapeLeft:
    case DisplayOrientation::LandscapeRight:
        break;
    default:
        throw std::runtime_error { "snapshot orientation is invalid" };
    }
    FrameFilePresenter writer { request.path };
    writer.present(frame);
    if (request.sequence)
        output_.marker(
            "[control] snapshot-sequence frame=" + request.path.string());
    else
        output_.marker("[control] snapshot=" + request.path.string() +
                       " frame=" + std::to_string(frame.sequence));
}

void SessionSnapshots::submit(
    const std::filesystem::path& path, DisplayFrame frame, bool sequence)
{
    try {
        if (pending_.size() >= maximum_pending)
            throw std::runtime_error { "snapshot request queue is full" };
        auto bytes = frame_bytes(frame.width, frame.height);
        if (frame.read_pixels)
            throw std::runtime_error {
                "snapshot source retains a materializer"
            };
        auto request = std::make_unique<Pending>();
        request->path = path;
        request->sequence = sequence;
        if (!frame.host_surface && pending_.empty()) {
            request->source = std::move(frame);
            publish(*request, request->source);
            return;
        }
        if (frame.host_surface) {
            if (!renderer_)
                throw std::runtime_error { "snapshot renderer is unavailable" };
            const auto descriptor = frame.host_surface->descriptor();
            bytes = std::max(
                bytes, frame_bytes(descriptor.width, descriptor.height));
            auto host = std::make_shared<GlesValueResult<SurfaceCpuReadback>>();
            host->value.renderer = renderer_;
            host->value.surface = frame.host_surface;
            // No reader, kernel, file path, or temporary call reaches the
            // worker. Retain the original content lease even if the observer is
            // canceled.
            host->completion = enqueue_gles_host_work(
                bytes, [host, lease = frame.presentation_lease](DisplayFrame&) {
                    static_cast<void>(lease);
                    return host->value.capture();
                });
            if (!host->completion)
                throw std::runtime_error {
                    "snapshot renderer capacity exhausted"
                };
            request->gpu = std::move(host);
        } else {
            if (frame.pixels.capacity() > pixel_limit / sizeof(std::uint32_t))
                throw std::length_error {
                    "snapshot CPU storage exceeds budget"
                };
            request->credit = reserve_gles_host_preparation(
                frame.pixels.capacity() * sizeof(std::uint32_t));
            if (!request->credit)
                throw std::runtime_error {
                    "snapshot CPU payload budget exhausted"
                };
        }
        request->source = std::move(frame);
        pending_.push_back(std::move(request));
    } catch (const std::exception& error) {
        fail(path, error.what());
    }
}

void SessionSnapshots::poll()
{
    while (!pending_.empty()) {
        const auto& first = pending_.front();
        if (first->gpu && !first->gpu->completion->ready())
            return;
        auto request = std::move(pending_.front());
        pending_
            .pop_front(); // Exactly one file publication, before callbacks/IO.
        try {
            DisplayFrame frame;
            if (request->gpu) {
                auto& host = request->gpu;
                if (!host->completion->succeeded())
                    throw std::runtime_error { "snapshot GPU capture failed" };
                const auto& captured = host->value.snapshot;
                if (captured.frame.pixels.size() !=
                    frame_bytes(request->source.width, request->source.height) /
                        sizeof(std::uint32_t))
                    throw std::runtime_error {
                        "snapshot source geometry changed"
                    };
                if (host->value.surface->commit_cpu_readback(captured))
                    host->value.renderer->acknowledge_readback(
                        host->value.surface->key(), host->value.stamp);
                frame = std::move(host->value.snapshot.frame);
                // Match DisplayState::snapshot's panel geometry and orientation
                // contract, including native sources with rotated dimensions.
                frame.width = request->source.width;
                frame.height = request->source.height;
                frame.sequence = request->source.sequence;
                frame.orientation = request->source.orientation;
                frame.owner_process_id = request->source.owner_process_id;
            } else {
                frame = std::move(request->source);
            }
            // The file sink receives only pixels; it cannot initiate GPU work.
            frame.host_surface.reset();
            frame.presentation_lease.reset();
            frame.presentation_staging_surface.reset();
            frame.presentation_staging_lease.reset();
            frame.read_pixels = { };
            publish(*request, frame);
        } catch (const std::exception& error) {
            fail(request->path, error.what());
        }
    }
}

void SessionSnapshots::cancel()
{
    if (pending_.empty())
        return;
    output_.marker(
        "[control] snapshots cancelled=" + std::to_string(pending_.size()));
    pending_.clear();
}

} // namespace ilemu
