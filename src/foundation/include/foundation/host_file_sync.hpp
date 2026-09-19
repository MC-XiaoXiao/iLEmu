// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <atomic>
#include <memory>
#include <optional>

#include "foundation/host_resource_controller.hpp"

namespace ilemu {

class HostFileSyncRequest {
public:
    ~HostFileSyncRequest();
    // Empty while pending, zero on success, otherwise a host errno value.
    [[nodiscard]] std::optional<int> result() const;

private:
    friend class HostFileSynchronizer;
    explicit HostFileSyncRequest(int descriptor);
    int descriptor_ { -1 };
    std::atomic<int> error_ { -1 };
};

// Mandatory file persistence is independent of optional compile admission.
// One bounded worker services requests from all guest processes. Each request
// pins its file until the real host fsync has completed.
class HostFileSynchronizer {
public:
    ~HostFileSynchronizer();
    [[nodiscard]] std::shared_ptr<HostFileSyncRequest> synchronize(int descriptor);

private:
    HostResourceController worker_;
};

} // namespace ilemu
