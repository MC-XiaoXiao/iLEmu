// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace ilemu {
class SurfaceStore;
class UserlandHleCall;

// Core-only import observer. The existing registry owns the guest continuation;
// only SurfaceCpuReadback and its owning ticket cross the graphics queue.
class SurfaceGuestImport final {
public:
    using Completion = std::function<void(UserlandHleCall&, bool)>;
    static void run(UserlandHleCall& call, std::shared_ptr<SurfaceStore> store,
        std::uint32_t id, Completion completion);

private:
    struct Pending;
    static void resume(UserlandHleCall& call, std::shared_ptr<Pending> pending);
};
} // namespace ilemu
