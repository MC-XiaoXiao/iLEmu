/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/architectural_stop.hpp"
#include <cstdint>
#include <optional>
namespace ilemu::execution::arm64 {
struct TraceMetadata {
    std::optional<arm::ArchitecturalStop> stop;
    std::uint64_t maximum_ticks = 0;
    unsigned instructions = 0;
    bool closed = false;
    // A hot call or ISA exchange can justify recording its retired cycle.
    bool path_candidate = false;
    bool accesses_memory = false;
    // Every path reaching maximum_ticks returns a stop before a successor fetch.
    // Earlier exits have strictly smaller paid costs.
    bool ends_with_stop = false;
    std::optional<std::uint32_t> first_instruction;
    bool selectable_with(const TraceMetadata& other) const
    {
        return !stop && !other.stop && maximum_ticks == other.maximum_ticks &&
            instructions == other.instructions && closed == other.closed &&
            path_candidate == other.path_candidate &&
            accesses_memory == other.accesses_memory &&
            ends_with_stop == other.ends_with_stop &&
            first_instruction == other.first_instruction;
    }
};
}
