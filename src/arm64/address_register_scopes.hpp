/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_register_plan.hpp"
#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace ilemu::execution::arm64 {
// Compile-scoped lifetimes for bases formed inside a recorded path. Existing
// native bias slots are reused after the previous architectural value returns.
class AddressRegisterScopes {
public:
    struct Position {
        std::uint16_t allowed = 0, ending = 0;
    };
    explicit AddressRegisterScopes(std::pmr::memory_resource* memory)
        : positions_(memory) { }
    void resize(unsigned steps) { positions_.resize(steps + 1U); }
    void add(unsigned reg, unsigned begin, unsigned end)
    {
        const auto bit = static_cast<std::uint16_t>(1U << reg);
        for (auto n = begin; n < end; ++n)
            if ((positions_[n].allowed & bit) != 0)
                return;
        const auto bases = std::span { plan_.bases }.first(plan_.count);
        if (std::find(bases.begin(), bases.end(), reg) == bases.end()) {
            if (plan_.count == plan_.capacity)
                return;
            plan_.bases[plan_.count++] = reg;
        }
        for (auto n = begin; n < end; ++n)
            positions_[n].allowed |= bit;
        positions_[end].ending |= bit;
    }
    const AddressRegisterPlan& plan() const { return plan_; }
    Position at(unsigned step) const { return positions_[step]; }

private:
    AddressRegisterPlan plan_;
    std::pmr::vector<Position> positions_;
};
}
