// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "graphics/gles_renderer.hpp"

namespace ilemu {

// Host-only pixels and native provenance. Capturing does not publish a CPU
// cache, acknowledge native damage, or access a guest address space.
struct SurfaceCpuReadback {
    std::shared_ptr<GlesRenderer> renderer;
    std::shared_ptr<HostSurface> surface;
    HostSurface::CpuSnapshot snapshot { };
    GlesRenderer::ReadbackStamp stamp;
    std::optional<HostRectangle> damage;

    [[nodiscard]] bool capture()
    {
        if (!renderer || !surface ||
            !renderer->capture_readback(*surface, snapshot, stamp,
                PerfCpuMapReason::CoreSurface, &damage))
            return false;
        // An absent native target cannot satisfy GPU-newer contents using an
        // older CPU shadow. A live cached image needs no native stamp.
        return snapshot.gpu_generation <= snapshot.cpu_generation ||
               stamp.renderer != 0;
    }
};

} // namespace ilemu
