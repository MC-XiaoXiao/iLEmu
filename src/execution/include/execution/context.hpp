/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"

namespace ilemu::execution {
// Slot-local control and reservations survive backend changes and cooperative
// slices. The execution owner serializes use; only stop requests are concurrent.
struct ExecutorContext {
    StopRequests stops;
    std::unique_ptr<ExclusiveMonitor> exclusive;

    void bind_memory(MemoryAccess* memory)
    {
        if (exclusive && (!memory || !exclusive->bound_to(*memory)))
            exclusive.reset();
    }
    void clear_exclusive_state()
    {
        if (exclusive)
            exclusive->clear();
    }
};
}
