/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "trace_path.hpp"
#include "execution/run.hpp"
#include "arm/t32_decode.hpp"
#include <memory>
#include <vector>

namespace ilemu::execution::arm64 {
// Run-local observations of actually retired instructions. No entry, source,
// guest storage or lease is retained across a checked access or cache eviction.
class PathRecorder {
public:
    bool begin(std::shared_ptr<const CodeIdentity> identity,
        std::optional<std::uint64_t> generation, const CpuThreadState& state) noexcept
    {
        clear();
        if (!identity || !generation)
            return false;
        try {
            positions_.reserve(TracePath::maximum_steps + 1U);
            positions_.push_back(location(state));
        } catch (const std::bad_alloc&) {
            return false;
        }
        identity_ = std::move(identity);
        generation_ = generation;
        endian_ = state.cpsr & 0x200U;
        active_ = true;
        return true;
    }
    bool active() const noexcept { return active_; }
    bool complete() const noexcept { return complete_; }
    void retired(const CpuThreadState& state) noexcept
    {
        if (!active_)
            return;
        if ((state.cpsr & 0x200U) != endian_) {
            clear();
            return;
        }
        const auto next = location(state);
        positions_.push_back(next); // begin() reserved the complete bound.
        if (next == positions_.front()) {
            active_ = false;
            complete_ = true;
        } else if (positions_.size() == TracePath::maximum_steps + 1U)
            clear();
    }
    std::span<const TraceLocation> ready(
        const std::shared_ptr<const CodeIdentity>& identity,
        std::optional<std::uint64_t> generation, const CpuThreadState& state) const noexcept
    {
        if (!complete_ || identity != identity_ || generation != generation_ ||
            (state.cpsr & 0x200U) != endian_ || location(state) != positions_.front())
            return {};
        return positions_;
    }
    void clear() noexcept
    {
        active_ = complete_ = false;
        positions_.clear();
        identity_.reset();
        generation_.reset();
    }
private:
    static TraceLocation location(const CpuThreadState& state) noexcept
    {
        return { state.registers[15], (state.cpsr & 0x20U) != 0,
            arm::it_state(state.cpsr) };
    }
    std::vector<TraceLocation> positions_;
    std::shared_ptr<const CodeIdentity> identity_;
    std::optional<std::uint64_t> generation_;
    std::uint32_t endian_ = 0;
    bool active_ = false, complete_ = false;
};
}
