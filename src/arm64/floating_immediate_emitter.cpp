/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_emitter.hpp"
namespace ilemu::execution::arm64 {
void FloatingEmitter::immediate(
    const arm::FloatingImmediateOperands& v, bool unconditional)
{
    // Bitwise moves need no host FP environment or exception synchronization.
    if (vectors_.constant_is_known(v.destination, v.wide, v.imm8))
        return;
    const auto value = vectors_.constant(v.wide, v.imm8);
    if (v.wide)
        vectors_.write_d(v.destination / 2U, value);
    else
        vectors_.write_s(v.destination, value);
    if (unconditional)
        vectors_.remember_constant(v.destination, v.wide, v.imm8);
}
}
