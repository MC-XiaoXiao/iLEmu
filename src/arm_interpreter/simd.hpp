/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/arm_state.hpp"

namespace ilemu::execution {
void duplicate_vector(CpuThreadState&, const arm::VectorDuplicateOperands&);
void immediate_vector(CpuThreadState&, const arm::VectorImmediateOperands&);
void bitwise_vector(CpuThreadState&, const arm::VectorBitwiseOperands&);
void extract_vector(CpuThreadState&, const arm::VectorExtractOperands&);
void move_core_vector(CpuThreadState&, const arm::VectorCoreMoveOperands&);
}
