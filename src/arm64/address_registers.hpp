/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_register_plan.hpp"
#include <oaknut/oaknut.hpp>
#include <optional>
namespace ilemu::execution::arm64 {
// Invocation-local representation. Biases are private to each base, independent
// of bounded address-cache slot eviction. Every exit publishes guest values.
class AddressRegisters {
public:
    AddressRegisters(oaknut::VectorCodeGenerator&, const AddressRegisterPlan&);
    bool eligible(unsigned reg) const;
    bool active(unsigned reg) const { return (active_ & (1U << reg)) != 0; }
    unsigned mask() const { return active_; }
    void acquire(unsigned reg, oaknut::XReg bias);
    bool update(const arm::Instruction&);
    bool writeback(const arm::Instruction&);
    void read_guest(oaknut::WReg destination, unsigned reg) const;
    void restore(unsigned mask) const;
    void finish_group();
private:
    std::optional<unsigned> slot(unsigned reg) const;
    oaknut::VectorCodeGenerator& code_;
    AddressRegisterPlan plan_;
    unsigned active_ = 0;
};
}
