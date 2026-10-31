// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ilemu {
class SurfaceStore;
struct GraphicsCallLease;
class UserlandHleCall;

// Core-only import observer. The existing registry owns the guest continuation;
// only SurfaceCpuReadback and its owning ticket cross the graphics queue.
class SurfaceGuestImport final {
public:
    using Completion = std::function<void(UserlandHleCall&, bool)>;
    using ReadCompletion = std::function<void(UserlandHleCall&,
        std::optional<std::vector<std::uint32_t>>)>;
    static void run(UserlandHleCall& call, std::shared_ptr<SurfaceStore> store,
        std::uint32_t id, Completion completion,
        std::shared_ptr<GraphicsCallLease> lease = { });

    // Internal CPU consumers receive an owned image merged with current guest
    // edits. This does not publish GPU pixels into the guest allocation.
    static void read(UserlandHleCall& call, std::shared_ptr<SurfaceStore> store,
        std::uint32_t id, ReadCompletion completion,
        std::shared_ptr<GraphicsCallLease> lease = { });

private:
    struct Pending;
    static void start(UserlandHleCall& call, std::shared_ptr<Pending> pending,
        std::uint32_t id);
    static void resume(UserlandHleCall& call, std::shared_ptr<Pending> pending);
};
} // namespace ilemu
