/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include "code_regions.hpp"
#include "trace_path.hpp"
#include <array>
#include <memory>

namespace ilemu::execution::arm64 {
// Opaque dependencies only. Every use reacquires all pins in the current lease;
// no guest bytes, backing pointers or instruction locks survive in the cache.
class TraceViews {
public:
    static std::unique_ptr<TraceViews> capture(
        InstructionSource&, InstructionLease&, InstructionRegion root,
        std::uint32_t pc, bool thumb, unsigned it);
    static std::unique_ptr<TraceViews> capture_path(
        InstructionLease&, InstructionRegion root, const TracePath&);
    enum class EntryAuthorization { Prepared, Cached };
    bool pin(InstructionLease&, InstructionRegion root,
        EntryAuthorization = EntryAuthorization::Prepared) const noexcept;
    // Confirm only after all dependencies have been pinned for this invocation.
    // The cookie shares the entry's lifetime; cache erasure revokes table revisions
    // before reclaiming either the owner or this validation address.
    const std::uint64_t* continuation_validation(std::uint64_t cookie) noexcept
    {
        continuation_cookie_ = cookie;
        return &continuation_cookie_;
    }
    InstructionRegion region() const noexcept { return region_; }
    std::array<InstructionRegion, CodeRegions::capacity> regions() const noexcept
    {
        std::array<InstructionRegion, CodeRegions::capacity> result {};
        // InstructionRegion defaults to the entire address space, not empty.
        result.fill(InstructionRegion { 0, 0 });
        for (unsigned i = 0; i < count_; ++i)
            result[i] = views_[i].view.region;
        return result;
    }
    std::size_t retained_bytes() const noexcept
    {
        // Include a conservative allowance for the opaque identity tokens.
        return sizeof(*this) + views_.size() * 64U;
    }
private:
    struct Dependency {
        LeasedCodeView view;
        std::uint32_t probe = 0;
    };
    std::array<Dependency, CodeRegions::capacity> views_;
    unsigned count_ = 0;
    InstructionRegion region_;
    std::uint64_t continuation_cookie_ = 0;
};
}
