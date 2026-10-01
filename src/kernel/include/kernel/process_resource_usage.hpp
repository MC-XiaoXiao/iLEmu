// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "kernel/darwin_resource_abi.hpp"
#include "mach/xnu_task_statistics.hpp"

namespace ilemu {
class AddressSpace;

// ARM32 rusage: two timeval pairs, peak resident bytes, thirteen counters.
// Preserve native wire widths during collection and ruadd aggregation.
class ProcessResourceUsage {
public:
    static ProcessResourceUsage from_task(const XnuTaskStatistics& statistics,
        std::uint32_t maximum_resident_bytes);
    void add(const ProcessResourceUsage& other);
    [[nodiscard]] bool copyout(AddressSpace& memory, std::uint32_t address) const;

private:
    std::array<std::uint32_t, darwin::resource::rusage_arm32_word_count> words_ { };
};
} // namespace ilemu
