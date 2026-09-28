// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>

namespace ilemu {
struct TaskMemoryStatistics {
    std::uint64_t virtual_bytes;
    std::uint64_t resident_bytes;
    std::uint64_t maximum_resident_bytes;
};
using TaskMemoryStatisticsQuery =
    std::function<std::optional<TaskMemoryStatistics>(std::uint32_t)>;
} // namespace ilemu
