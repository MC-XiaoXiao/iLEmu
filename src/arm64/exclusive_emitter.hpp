/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
class ExclusiveEmitter {
public:
    explicit ExclusiveEmitter(oaknut::VectorCodeGenerator& code) : code_(code) { }
    void emit(const arm::Instruction&, std::uint64_t ticks, oaknut::Label& checked);
private:
    oaknut::VectorCodeGenerator& code_;
};
}
