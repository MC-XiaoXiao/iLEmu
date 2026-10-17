/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <array>
namespace ilemu::execution::arm64 {
// Address-only registers may carry page pointers or fixed entry-relative offsets.
// Unknown instructions conservatively revoke this representation for the cycle.
struct AddressRegisterPlan {
    static constexpr unsigned capacity = 4;
    std::array<unsigned, capacity> bases { };
    unsigned count = 0;
    unsigned deferred_mask = 0;
    static bool update(const arm::Instruction&);
    class Builder;
};
class AddressRegisterPlan::Builder {
public:
    void observe(const arm::Instruction&, bool addressable);
    bool permits(unsigned reg) const { return (forbidden_ & (1U << reg)) == 0; }
    bool permits_constant_offsets(unsigned reg) const
    {
        return permits(reg) && (variable_updates_ & (1U << reg)) == 0;
    }
private:
    void forbid(unsigned reg) { forbidden_ |= 1U << reg; }
    unsigned forbidden_ = 0;
    unsigned variable_updates_ = 0;
};
}
