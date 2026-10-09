/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/arm_state.hpp"

namespace ilemu::execution {
void duplicate_vector(CpuThreadState&, const arm::VectorDuplicateOperands&);
void bitwise_vector(CpuThreadState&, const arm::VectorBitwiseOperands&);
}
