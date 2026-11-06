// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace ilemu {
class GlesRenderer;
class HostSurface;

// An owned host shader target. Capture and publication stay on the host GPU;
// no driver handles cross into the guest graphics model.
class GlesProgramTarget {
public:
    enum class Api { None, AndroidHardwareBuffer, ExternalMemoryFd };
    using DeviceUuid = std::array<std::uint8_t, 16>;
    struct DeviceIdentity {
        DeviceUuid uuid, driver;
        bool operator==(const DeviceIdentity&) const = default;
    };
    struct Identity {
        Api api { Api::None };
        DeviceIdentity device { };
        bool operator==(const Identity&) const = default;
    };
    [[nodiscard]] Identity identity() const { return { api(), device_ }; }
    virtual ~GlesProgramTarget() = default;
    [[nodiscard]] virtual Api api() const = 0;
    [[nodiscard]] const std::shared_ptr<HostSurface>& surface() const
    {
        return surface_;
    }

protected:
    GlesProgramTarget(std::shared_ptr<GlesRenderer> owner,
        std::shared_ptr<HostSurface> surface, DeviceIdentity device = { })
        : owner_ { std::move(owner) }
        , surface_ { std::move(surface) }
        , device_ { device }
    {
    }
    // The imported allocation must be destroyed before its device.
    std::shared_ptr<GlesRenderer> owner_;
    std::shared_ptr<HostSurface> surface_;
    DeviceIdentity device_;
};
}
