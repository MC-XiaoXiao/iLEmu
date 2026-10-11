/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm_memory/transfer.hpp"
namespace ilemu::execution::arm_memory {
Completion complete_dual(CpuThreadState&, MemoryAccess&, const Transfer&);
}
