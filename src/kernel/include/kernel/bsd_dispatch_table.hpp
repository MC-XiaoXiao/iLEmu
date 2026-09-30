// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "kernel/syscall_routes.hpp"

namespace ilemu {
class CompatibilityKernel;
class Cpu;

// Immutable executable bindings. Resolve gates, aliases and error policy once.
class BsdDispatchTable {
public:
    explicit BsdDispatchTable(
        const DarwinAbi& abi, std::string_view darwin_release = { });
    void dispatch(
        CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) const;
    [[nodiscard]] bool sends_sigsys(std::uint32_t number) const;

private:
    using Adapter = void (*)(CompatibilityKernel&, Cpu&, std::uint32_t);
    struct Binding {
        Adapter adapter { };
        std::uint16_t canonical { };
        bool send_sigsys { };
        bool trace_unknown { true };
    };
    static Adapter adapter_for(syscall_routes::Handler handler);
    static void io_policy(
        CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number);
    const bool default_sigsys_;
    std::array<Binding, syscall_routes::Table::bsd_capacity> bindings_ { };
};
} // namespace ilemu
