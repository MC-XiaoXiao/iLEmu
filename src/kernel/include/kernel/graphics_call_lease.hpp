// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "graphics/gles_program_renderer.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace ilemu {
// Core-only store dependency. Worker publication alone does not let another
// call pass: the original observer must consume its result, or cancellation
// must wait for real accepted work before retiring ownership.
struct GraphicsCallLease {
    std::size_t owner { };
    bool consumed { }, cancelled { };
    std::shared_ptr<GlesWorkResult> result;
    // Core-only shared-object commit, also run if the issuing thread exits.
    // A whole-process exit/exec discards it instead.
    std::function<void()> complete_core;
    [[nodiscard]] bool ready() const
    {
        return consumed || (cancelled && (!result || result->ready()));
    }
    void consume()
    {
        if (consumed)
            return;
        consumed = true;
        // Release ownership even if a core metadata commit fails. Keep
        // the ticket alive throughout that commit, outside the lease.
        const auto retained = std::move(result);
        auto completion = std::move(complete_core);
        if (completion)
            completion();
    }
};
} // namespace ilemu
