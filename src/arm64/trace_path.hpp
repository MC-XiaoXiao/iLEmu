/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

namespace ilemu::execution::arm64 {
struct TraceLocation {
    std::uint32_t pc;
    bool thumb;
    unsigned it;
    auto operator<=>(const TraceLocation&) const = default;
};
// A compile-scoped policy learned from retired positions, never guest values.
// Every successor is checked against the freshly decoded instruction.
class TracePath {
public:
    enum class Edge { Sequential, Taken, NotTaken, Dynamic };
    static constexpr unsigned initial_steps = 512;
    static constexpr unsigned maximum_steps = 4096;
    explicit TracePath(std::span<const TraceLocation> locations = {})
        : locations_(locations)
    {
        if (!locations.empty() && (locations.size() < 2 ||
                locations.size() > maximum_steps + 1U ||
                locations.front() != locations.back()))
            throw std::invalid_argument("invalid closed instruction path");
    }
    unsigned steps() const
    {
        return locations_.empty() ? 0U : static_cast<unsigned>(locations_.size() - 1U);
    }
    explicit operator bool() const { return steps() != 0; }
    const TraceLocation& operator[](unsigned n) const { return locations_[n]; }
    std::optional<Edge> edge(unsigned n, const arm::Instruction& inst,
        unsigned next_it, bool dynamic) const
    {
        if (n >= steps())
            return {};
        const auto& at = locations_[n];
        const auto& next = locations_[n + 1U];
        const TraceLocation linear { at.pc + inst.size, at.thumb, next_it };
        if (inst.kind == arm::InstructionKind::Branch ||
            inst.kind == arm::InstructionKind::CompareBranch) {
            const auto target = ((at.pc + inst.pc_offset) &
                (inst.align_pc ? ~3U : ~0U)) + inst.immediate;
            const TraceLocation taken { target, inst.exchange ? !at.thumb : at.thumb,
                next_it };
            if ((!inst.exchange || next_it == 0) && next == taken)
                return Edge::Taken;
            if (next == linear && (inst.condition != 14 ||
                    inst.kind == arm::InstructionKind::CompareBranch))
                return Edge::NotTaken;
            return {};
        }
        if (dynamic) {
            if (inst.condition != 14 && next == linear)
                return Edge::NotTaken;
            if ((n + 1U != steps() &&
                    inst.kind != arm::InstructionKind::BranchExchange) ||
                next.it != next_it || (!next.thumb && next.it != 0) ||
                (next.pc & (next.thumb ? 1U : 3U)) != 0)
                return {};
            return Edge::Dynamic;
        }
        return next == linear ? std::optional { Edge::Sequential } : std::nullopt;
    }
private:
    std::span<const TraceLocation> locations_;
};
}
