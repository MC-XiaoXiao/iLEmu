// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
namespace ilemu {
// Host HLE time is not guest kernel CPU time.
struct XnuTaskStatistics {
    std::uint64_t terminated_user_ticks { };
    std::uint64_t live_user_ticks { };
    std::uint64_t live_user_microseconds { };
    std::uint32_t ticks_per_second { };
    std::uint32_t context_switches { };
};
using TaskStatisticsQuery =
    std::function<std::optional<XnuTaskStatistics>(std::uint32_t, bool)>;
}
