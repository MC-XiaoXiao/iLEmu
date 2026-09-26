// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "catalog_internal.hpp"
#include "device_state/pthread_contract.hpp"
#include <initializer_list>
#include <stdexcept>
#include <string>
namespace ilemu::syscall_routes {
namespace {
    template <class T>
    void require_known(
        T value, std::initializer_list<T> values, const char* field)
    {
        for (const auto candidate : values)
            if (candidate == value)
                return;
        throw std::invalid_argument(
            std::string("unknown route contract: ") + field);
    }
    void validate_contracts(const DarwinAbi& abi)
    {
        require_known(abi.abi_epoch,
            { DarwinAbiEpoch::IphoneOs1, DarwinAbiEpoch::IphoneOs2,
                DarwinAbiEpoch::IphoneOs3, DarwinAbiEpoch::Darwin10,
                DarwinAbiEpoch::Darwin11, DarwinAbiEpoch::Darwin13,
                DarwinAbiEpoch::Later },
            "abi_epoch");
        static_cast<void>(resolve_pthread_contract(abi.pthread_abi));
        require_known(abi.psynch_abi,
            { DarwinPsynchAbi::Unsupported,
                DarwinPsynchAbi::Arm32GenerationV1 },
            "psynch_abi");
        require_known(abi.sysctl_by_name_abi,
            { DarwinSysctlByNameAbi::LegacySemaphoreValue,
                DarwinSysctlByNameAbi::NamedSysctlAt274 },
            "sysctl_by_name_abi");
        require_known(abi.shared_region_abi,
            { DarwinSharedRegionAbi::LegacyRelocatableMappings,
                DarwinSharedRegionAbi::FixedMappingsWithSlideInfoV1 },
            "shared_region_abi");
        require_known(abi.coalition_abi,
            { DarwinCoalitionAbi::Unsupported,
                DarwinCoalitionAbi::ResourceCoalitions },
            "coalition_abi");
        require_known(abi.stack_snapshot_abi,
            { DarwinStackSnapshotAbi::Unsupported,
                DarwinStackSnapshotAbi::LegacyFourArguments,
                DarwinStackSnapshotAbi::LegacyFiveArguments },
            "stack_snapshot_abi");
        require_known(abi.mach_kernel_rpc,
            { DarwinMachKernelRpcAbi::LegacyMigOnly,
                DarwinMachKernelRpcAbi::DirectVmAndPortTrapsV1,
                DarwinMachKernelRpcAbi::DirectWideVmAndPortTraps },
            "mach_kernel_rpc");
    }
}
Table build(const DarwinAbi& abi)
{
    validate_contracts(abi);
    Table table;
    bind_bsd_entries(table, abi);
    bind_pthread_entries(table, abi);
    bind_mach_entries(table, abi);
    bind_alias_entries(table);
    table.validate();
    return table;
}
}
