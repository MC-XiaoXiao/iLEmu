// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "kernel/syscall_routes.hpp"

namespace ilemu {
class CompatibilityKernel;
class Cpu;
// Immutable per-session bindings; no reference lookup on the trap hot path.
class MachDispatchTable {
public:
    explicit MachDispatchTable(const DarwinAbi& abi, std::string_view abi_profile = { });
    void dispatch(CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) const;
private:
    using Adapter = void (*)(CompatibilityKernel&, Cpu&, std::uint32_t);
    struct Binding {
        Adapter adapter { };
        std::uint32_t result { 4 }; // kern_invalid
        bool trace_unknown { true };
    };
    static Adapter adapter_for(syscall_routes::Handler handler);
    std::array<Binding, syscall_routes::Table::mach_capacity> bindings_ { };
};
} // namespace ilemu
