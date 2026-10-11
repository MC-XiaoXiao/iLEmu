/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
void emit_simd_immediate(oaknut::VectorCodeGenerator&, oaknut::QReg,
    const arm::VectorImmediateOperands&);
}
