// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "catalog_internal.hpp"
#include "device_state/pthread_contract.hpp"
namespace ilemu::syscall_routes {
void bind_pthread_entries(Table& table, const DarwinAbi& abi)
{
    const auto& contract = resolve_pthread_contract(abi.pthread_abi);
    const auto add = [&](std::uint32_t number, std::string_view name,
                         Contract required, bool supported) {
        table.bind_new({ Domain::BsdSyscall, number, number, name,
            Handler::BsdPthread, required, Cancellation::OriginalEntry,
            supported ? Outcome::HandlerValidated : Outcome::BsdUnknown,
            "bsd/pthread_runtime.cpp" });
    };
    const bool v1 = contract.supports_registration_v1();
    add(360, "bsdthread_create", Contract::PthreadRegisterV1, v1);
    add(361, "bsdthread_terminate", Contract::PthreadRegisterV1, v1);
    add(366, "bsdthread_register", Contract::PthreadRegisterV1, v1);
    add(367, "workq_open", Contract::PthreadRegisterV1, v1);
    add(368, "workq_kernreturn", Contract::PthreadRegisterV1, v1);
    add(478, "bsdthread_ctl", Contract::PthreadControl,
        v1 && contract.supports_bsdthread_ctl);
    add(372, "thread_selfid", Contract::PthreadIdentity,
        v1 || contract.supports_thread_identity);
}
}
