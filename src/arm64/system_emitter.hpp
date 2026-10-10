/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
class SystemEmitter {
public:
    explicit SystemEmitter(oaknut::VectorCodeGenerator& code) : code_(code) { }
    void emit(const arm::Instruction&);
private:
    oaknut::VectorCodeGenerator& code_;
};
}
