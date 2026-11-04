// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <memory>

namespace ilemu {
class GlesRenderer;
class HostSurface;

// An owned host shader target. Capture and publication stay on the host GPU;
// no driver handles cross into the guest graphics model.
class GlesProgramTarget {
public:
    enum class Api { None, AndroidHardwareBuffer };
    virtual ~GlesProgramTarget() = default;
    [[nodiscard]] virtual Api api() const = 0;
    [[nodiscard]] const std::shared_ptr<HostSurface>& surface() const
    {
        return surface_;
    }

protected:
    GlesProgramTarget(std::shared_ptr<GlesRenderer> owner,
        std::shared_ptr<HostSurface> surface)
        : owner_ { std::move(owner) }
        , surface_ { std::move(surface) }
    {
    }
    // The imported allocation must be destroyed before its device.
    std::shared_ptr<GlesRenderer> owner_;
    std::shared_ptr<HostSurface> surface_;
};
}
