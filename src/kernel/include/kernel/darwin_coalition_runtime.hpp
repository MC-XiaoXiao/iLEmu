// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Track Darwin resource-coalition identifiers across guest processes.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>

namespace ilemu {

class DarwinCoalitionRuntime {
public:
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
        bool privileged { };
        bool terminated { };
        std::uint32_t active_count { };
    };

    std::mutex mutex_;
    // XNU allocates ID 1 to its privileged default coalition at startup.
    std::uint64_t next_identifier_ { 2 };
    std::map<std::uint64_t, Coalition> coalitions_;
};

} // namespace ilemu
