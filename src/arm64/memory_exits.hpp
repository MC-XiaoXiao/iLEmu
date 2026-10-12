/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
// Resolve a memory exit only when generated code actually references it.
// The provider owns the instruction's precise retirement and fault payload.
class MemoryExits {
public:
    enum class Kind { Checked, Unsupported, Branch };
    virtual ~MemoryExits() = default;
    virtual oaknut::Label& get(Kind) = 0;
};
}
