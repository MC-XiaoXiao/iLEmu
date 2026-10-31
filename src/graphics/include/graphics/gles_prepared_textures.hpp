// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "graphics/gles_resources.hpp"
#include "graphics/surface_cpu_readback.hpp"

namespace ilemu {
class SurfaceTransportLease;

// Frozen inputs owned by a single draw packet. Only capture() runs on the
// worker; complete() consumes its published pixels at a serial core boundary.
class GlesPreparedTextures {
public:
    [[nodiscard]] static std::size_t payload_bytes(
        const GlesResourceStore& source, std::span<const std::uint32_t> names,
        std::size_t limit);
    GlesPreparedTextures() = default;
    GlesPreparedTextures(const GlesResourceStore& source,
        std::span<const std::uint32_t> names, const SurfaceStore& surfaces);
    [[nodiscard]] bool needs_capture() const { return !sources_.empty(); }
    [[nodiscard]] bool capture(const std::shared_ptr<GlesRenderer>& renderer);
    void complete(GlesResourceStore& revisions);
    [[nodiscard]] const GlesResourceStore& resources() const
    {
        return textures_;
    }

private:
    struct Source {
        std::uint32_t name, level;
        std::uint64_t revision;
        std::shared_ptr<GlesTextureTarget> native_lifetime;
        std::shared_ptr<SurfaceTransportLease> transport;
        SurfaceCpuReadback readback;
    };
    GlesResourceStore textures_;
    std::vector<Source> sources_;
    bool completed_ { };
};
} // namespace ilemu
