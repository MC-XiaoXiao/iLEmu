/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <optional>
namespace ilemu::execution::arm64 {
// Compile-only reservations in the existing constant pool and unused banks.
struct ScalarRegisterPlan {
    static constexpr unsigned constant_capacity = 5;
    static constexpr unsigned capacity = constant_capacity + 16;
    struct Entry {
        unsigned word = 0, host = 0;
        bool wide = false, written = false;
    };
    std::array<Entry, capacity> entries { };
    unsigned count = 0;
    // Reserve existing constant slots for single/double accumulation units.
    unsigned accumulation_units = 0;
    std::optional<unsigned> find(unsigned word, bool wide) const;
    unsigned constant_slots() const
    {
        return std::min(count, constant_capacity -
            static_cast<unsigned>(std::popcount(accumulation_units)));
    }
    class Builder;
};
class ScalarRegisterPlan::Builder {
public:
    void observe(const arm::FloatingArithmeticOperands&);
    void observe(const arm::VectorTransferOperands&, bool load);
    ScalarRegisterPlan finish() const;

private:
    struct Use {
        unsigned weight = 0;
        bool present = false, written = false, whole = false;
    };
    void use(unsigned word, bool wide, bool write, unsigned bonus = 0,
        bool whole = false);
    std::array<bool, 64> blocked_ { };
    std::array<Use, 64> singles_ { };
    std::array<Use, 32> doubles_ { };
    std::array<unsigned, 2> product_signs_ { };
    unsigned negative_accumulators_ = 0;
};
}
