// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "graphics/gles_rasterizer.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace ilemu {
struct DisplayFrame;
struct HostSurfaceKey;
struct GlesWorkResult;
class GlesRenderer;

// Immutable fixed-pipeline input for a CPU-owned target frame. Shared/native
// sources require their own ordered writer dependency before using this path.
class GlesFrameDraw final {
public:
    [[nodiscard]] static bool can_snapshot(
        const DisplayFrame& frame, const GlesRasterState& raster);
    [[nodiscard]] static std::shared_ptr<GlesFrameDraw> prepare(
        const DisplayFrame& frame, HostSurfaceKey target,
        std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
        const GlesRasterState& raster, std::size_t& bytes);
    [[nodiscard]] bool capacity() const;
    // Retry returns the accepted ticket, never another draw submission.
    [[nodiscard]] std::shared_ptr<GlesWorkResult> enqueue(
        std::shared_ptr<GlesRenderer> renderer);

private:
    struct State;
    explicit GlesFrameDraw(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
    std::shared_ptr<GlesWorkResult> result_;
};
} // namespace ilemu
