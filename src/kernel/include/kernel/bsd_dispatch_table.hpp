// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "kernel/syscall_routes.hpp"

namespace ilemu {
class CompatibilityKernel;
class Cpu;

// Session-owned executable bindings for migrated BSD entries only.
class BsdDispatchTable {
public:
    explicit BsdDispatchTable(const DarwinAbi& abi);
    [[nodiscard]] bool dispatch(CompatibilityKernel& kernel, Cpu& cpu,
        std::uint32_t number) const;

private:
    using Adapter = void (*)(CompatibilityKernel&, Cpu&, std::uint32_t);
    static Adapter adapter_for(syscall_routes::Handler handler);
    std::array<Adapter, syscall_routes::Table::bsd_capacity> adapters_ {};
};
} // namespace ilemu
