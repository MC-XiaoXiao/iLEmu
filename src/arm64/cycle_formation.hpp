/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "compiler.hpp"
#include <limits>
#include <optional>

namespace ilemu::execution::arm64 {
// The ordinary copy policy is final after the steady second cycle. A caller
// can complete another checked lowering before materializing unused copies.
class CycleFormation {
public:
    virtual ~CycleFormation() = default;
    std::optional<CompiledTrace> try_complete(const TraceMetadata& prefix,
        unsigned cycle_instructions, unsigned copies, std::uint64_t budget)
    {
        if (copies < 4 || cycle_instructions == 0 ||
            prefix.instructions != std::uint64_t { cycle_instructions } * 2U ||
            prefix.maximum_ticks == 0 || prefix.maximum_ticks % 2U != 0)
            return {};
        const auto ticks = prefix.maximum_ticks / 2U;
        const auto instructions = std::uint64_t { cycle_instructions } * copies;
        if (copies > budget / ticks || instructions > std::numeric_limits<unsigned>::max())
            return {};
        auto group = prefix;
        group.maximum_ticks = ticks * copies;
        group.instructions = static_cast<unsigned>(instructions);
        group.closed = true;
        return build(group);
    }

protected:
    virtual std::optional<CompiledTrace> build(const TraceMetadata&) = 0;
};
}
