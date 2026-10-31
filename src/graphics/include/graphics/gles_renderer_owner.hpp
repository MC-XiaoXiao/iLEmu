// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ilemu {
class GlesRenderer;
struct HostSurfaceKey;

// Core publisher for one renderer namespace or shared target. Admission reserves the
// retirement record before resource use; destruction only posts maintenance.
// No guest reference or kernel callback is retained by its host worker record.
class GlesRendererOwner final {
public:
    static constexpr std::size_t maximum_namespaces = 1024;
    GlesRendererOwner(
        std::shared_ptr<GlesRenderer> renderer, std::uint64_t owner);
    // Shared surfaces retire a single publication key through the same lane.
    GlesRendererOwner(
        std::shared_ptr<GlesRenderer> renderer, HostSurfaceKey target);
    ~GlesRendererOwner();
    GlesRendererOwner(const GlesRendererOwner&) = delete;
    GlesRendererOwner& operator=(const GlesRendererOwner&) = delete;

    [[nodiscard]] bool activate();
    [[nodiscard]] bool active() const;
    [[nodiscard]] static bool capacity();

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace ilemu
