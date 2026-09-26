// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/bsd_dispatch_table.hpp"
#include "kernel/kernel.hpp"

#include <stdexcept>

namespace ilemu {
BsdDispatchTable::Adapter BsdDispatchTable::adapter_for(
    syscall_routes::Handler handler)
{
    using syscall_routes::Handler;
    switch (handler) {
    case Handler::BsdPosixSemaphore:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            kernel.dispatch_bsd_posix_semaphore(cpu, number);
        };
    case Handler::BsdEvents:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            kernel.dispatch_bsd_events(cpu, number);
        };
    case Handler::BsdSharedRegion:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_shared_region(cpu, number));
        };
    case Handler::BsdPsynch:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            kernel.dispatch_bsd_psynch(cpu, number);
        };
    default:
        throw std::logic_error("missing migrated BSD handler adapter");
    }
}

BsdDispatchTable::BsdDispatchTable(const DarwinAbi& abi)
{
    const auto table = syscall_routes::build_bsd_contracts(abi);
    for (const auto& slot : table.entries(syscall_routes::Domain::BsdSyscall)) {
        if (!slot)
            continue;
        const auto& entry = *slot;
        if (entry.outcome != syscall_routes::Outcome::HandlerValidated ||
            entry.number != entry.canonical_number ||
            entry.cancellation != syscall_routes::Cancellation::OriginalEntry)
            throw std::logic_error("unsupported migrated BSD entry contract");
        adapters_[entry.number] = adapter_for(entry.handler);
    }
}

bool BsdDispatchTable::dispatch(
    CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) const
{
    if (number >= adapters_.size() || !adapters_[number])
        return false;
    adapters_[number](kernel, cpu, number);
    return true;
}
} // namespace ilemu
