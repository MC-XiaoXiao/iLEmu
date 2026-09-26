// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "catalog_internal.hpp"
#include <stdexcept>
namespace ilemu::syscall_routes {
void bind_bsd_contract_entries(Table& table, const DarwinAbi& abi)
{
    switch (abi.sysctl_by_name_abi) {
    case DarwinSysctlByNameAbi::LegacySemaphoreValue:
    case DarwinSysctlByNameAbi::NamedSysctlAt274:
        break;
    default:
        throw std::invalid_argument(
            "unknown route contract: sysctl_by_name_abi");
    }
    // Checked replacements model the current outer-switch precedence.
    const Entry semaphore { Domain::BsdSyscall, 274, 274,
        "posix_semaphore_get_value", Handler::BsdPosixSemaphore,
        Contract::SemaphoreValue, Cancellation::OriginalEntry,
        Outcome::HandlerValidated, "bsd/dispatch.cpp" };
    table.bind_new(semaphore);
    if (abi.sysctl_by_name_abi == DarwinSysctlByNameAbi::NamedSysctlAt274) {
        auto named = semaphore;
        named.operation = "sysctlbyname";
        named.handler = Handler::BsdEvents;
        named.contract = Contract::NamedSysctl;
        table.replace_entry(semaphore, named);
    }
}
Table build_bsd_contracts(const DarwinAbi& abi)
{
    Table table;
    bind_bsd_contract_entries(table, abi);
    table.validate();
    return table;
}
}
