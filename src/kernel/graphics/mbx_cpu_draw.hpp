// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "graphics/display.hpp"
#include "kernel/mbx2d_hle.hpp"

namespace ilemu {
struct Mbx2dHle::CpuDraw {
    struct Output {
        std::vector<std::uint32_t> pixels;
        std::optional<BlendState> blend;
        HostRectangle rectangle { };
        std::optional<QuadCoverage> coverage;
        [[nodiscard]] std::optional<std::vector<std::uint32_t>> merge(
            std::vector<std::uint32_t> destination) const;
    };
    // Worker input contains host data and lifetimes only, never CpuDraw's
    // guest mappings or its core continuation. Output is immutable after
    // admission.
    struct Framebuffer {
        std::shared_ptr<void> credit;
        // Null output captures a source without merging a destination.
        std::shared_ptr<const Output> output;
        DisplayFrame source;
        std::shared_ptr<GlesWorkResult> result;
        std::uint64_t revision { }, generation { };
        bool execute(
            const std::shared_ptr<GlesRenderer>& renderer, DisplayFrame& frame);
    };
    // Credit outlives its owned result, including cancellation while parked.
    std::shared_ptr<void> credit;
    std::shared_ptr<Output> output;
    std::shared_ptr<Framebuffer> framebuffer;
    std::optional<ResolvedSurface> source;
    ResolvedSurface destination;
    AddressSpace::MappingObservation source_mapping, destination_mapping;
    std::shared_ptr<DisplayState> display;
    std::shared_ptr<GraphicsCallLease> lease;
    CpuContinuation continuation;
    std::uint64_t owner { };
    std::size_t bytes { }, output_bytes { };
    bool context_api { };
};
} // namespace ilemu
