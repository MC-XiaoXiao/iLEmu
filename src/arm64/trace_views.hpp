/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include <array>
#include <memory>

namespace ilemu::execution::arm64 {
// Opaque dependencies only. Every use reacquires all pins in the current lease;
// no guest bytes, backing pointers or instruction locks survive in the cache.
class TraceViews {
public:
    static std::unique_ptr<TraceViews> capture(
        InstructionLease&, InstructionRegion root, std::uint32_t pc);
    enum class EntryAuthorization { Prepared, Cached };
    bool pin(InstructionLease&, InstructionRegion root,
        EntryAuthorization = EntryAuthorization::Prepared) const noexcept;
    InstructionRegion region() const noexcept { return region_; }
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
    std::array<Dependency, 4> views_;
    unsigned count_ = 0;
    InstructionRegion region_;
};
}
