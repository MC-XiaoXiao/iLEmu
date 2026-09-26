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
    switch (abi.psynch_abi) {
    case DarwinPsynchAbi::Unsupported:
    case DarwinPsynchAbi::Arm32GenerationV1:
        break;
    default:
        throw std::invalid_argument("unknown route contract: psynch_abi");
    }
    const auto bind_region = [&](std::uint32_t number, std::string_view legacy,
                                 std::string_view psynch) {
        const Entry expected { Domain::BsdSyscall, number, number, legacy,
            Handler::BsdSharedRegion, Contract::LegacySharedRegion,
            Cancellation::OriginalEntry, Outcome::HandlerValidated,
            "bsd/dispatch.cpp" };
        table.bind_new(expected);
        if (abi.psynch_abi == DarwinPsynchAbi::Arm32GenerationV1) {
            auto entry = expected;
            entry.operation = psynch;
            entry.handler = Handler::BsdPsynch;
            entry.contract = Contract::Psynch;
            table.replace_entry(expected, entry);
        }
    };
    bind_region(299, "shared_region_map_file_np", "psynch_rw_downgrade");
    bind_region(300, "shared_region_make_private_np", "psynch_rw_upgrade");
}
Table build_bsd_contracts(const DarwinAbi& abi)
{
    Table table;
    bind_bsd_contract_entries(table, abi);
    table.validate();
    return table;
}
}
