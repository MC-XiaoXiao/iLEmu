/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
class SimdEmitter {
public:
    explicit SimdEmitter(oaknut::VectorCodeGenerator& code) : code_(code) { }
    void duplicate(const arm::VectorDuplicateOperands&);

private:
    oaknut::VectorCodeGenerator& code_;
};
}
