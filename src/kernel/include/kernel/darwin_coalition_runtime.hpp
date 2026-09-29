// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Track Darwin resource-coalition identifiers across guest processes.

#pragma once

#include "device_state/darwin_abi.hpp"

#include <cstdint>
#include <map>
#include <mutex>

namespace ilemu {

class DarwinCoalitionRuntime {
public:
    explicit DarwinCoalitionRuntime(
        DarwinCoalitionAbi abi = DarwinCoalitionAbi::ResourceCoalitions);
    struct CreateResult {
        std::uint32_t error { };
        std::uint64_t identifier { };
    };

    [[nodiscard]] CreateResult create(std::uint32_t flags);
    [[nodiscard]] std::uint32_t request_terminate(
        std::uint64_t identifier, std::uint32_t flags);
    [[nodiscard]] std::uint32_t reap(
        std::uint64_t identifier, std::uint32_t flags);

private:
    struct Coalition {
        std::uint32_t type { };
        bool privileged { };
        bool terminated { };
        std::uint32_t active_count { };
    };

    std::mutex mutex_;
    const std::uint32_t type_count_;
    // Each supported type has a privileged default coalition at startup.
    std::uint64_t next_identifier_;
    std::map<std::uint64_t, Coalition> coalitions_;
};

} // namespace ilemu
