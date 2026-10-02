// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <deque>
#include <utility>

namespace ilemu {

// The wait primitive is independent of its lifetime owner: Mach receive
// rights own port semaphores; POSIX names, file references and in-flight
// waits jointly own named semaphores. Access is serialized by mach_mutex.
struct SemaphoreState {
    std::int64_t count { };
    std::uint32_t owner_pid { };
    std::deque<std::pair<std::uint32_t, std::uint32_t>> waiters;
};

} // namespace ilemu
