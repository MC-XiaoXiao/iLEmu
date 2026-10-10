/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
namespace ilemu::arm {
// Decode the unprivileged system-control subset, shared by A32 and T32.
bool decode_system_control(Instruction&, std::uint32_t, bool thumb) noexcept;
}
