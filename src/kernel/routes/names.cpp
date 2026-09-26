// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/syscall_routes.hpp"
namespace ilemu::syscall_routes {
std::string_view handler_name(Handler value)
{
    switch (value) {
    case Handler::BsdProcess:
        return "dispatch_bsd_process";
    case Handler::BsdProcessSockets:
        return "dispatch_bsd_process_sockets";
    case Handler::BsdPosixSemaphore:
        return "dispatch_bsd_posix_semaphore";
    case Handler::BsdSignal:
        return "dispatch_bsd_signal";
    case Handler::BsdPlatform:
        return "dispatch_bsd_platform";
    case Handler::BsdFilesystem:
        return "dispatch_bsd_filesystem";
    case Handler::BsdDescriptorMemory:
        return "dispatch_bsd_descriptor_memory";
    case Handler::BsdSharedRegion:
        return "dispatch_bsd_shared_region";
    case Handler::BsdPsynch:
        return "dispatch_bsd_psynch";
    case Handler::BsdAio:
        return "dispatch_bsd_aio";
    case Handler::BsdDebug:
        return "dispatch_bsd_debug";
    case Handler::BsdSocket:
        return "dispatch_bsd_socket";
    case Handler::BsdEvents:
        return "dispatch_bsd_events";
    case Handler::BsdKqueue:
        return "dispatch_bsd_kqueue";
    case Handler::BsdCodeSigning:
        return "dispatch_bsd_code_signing";
    case Handler::BsdSecurity:
        return "dispatch_bsd_security";
    case Handler::BsdAuditSession:
        return "dispatch_bsd_audit_session";
    case Handler::BsdFileport:
        return "dispatch_bsd_fileport";
    case Handler::BsdGuardedFile:
        return "dispatch_bsd_guarded_file";
    case Handler::BsdCoalition:
        return "dispatch_bsd_coalition";
    case Handler::BsdNetworkPolicy:
        return "dispatch_bsd_network_policy";
    case Handler::BsdDirectoryAttributes:
        return "dispatch_bsd_directory_attributes";
    case Handler::BsdPthread:
        return "dispatch_bsd_pthread";
    case Handler::BsdLedgerInline:
        return "dispatch_bsd/ledger";
    case Handler::BsdIoPolicyInline:
        return "dispatch_bsd/iopolicysys";
    case Handler::BsdStackSnapshotInline:
        return "dispatch_bsd/stack_snapshot";
    case Handler::MachInline:
        return "dispatch_mach/inline";
    case Handler::MachMessage:
        return "dispatch_mach_message";
    case Handler::MachThreadSelf:
        return "dispatch_mach_thread_self_trap";
    case Handler::MachVmRpc:
        return "dispatch_mach_vm_kernel_rpc_trap";
    case Handler::MachPortRpc:
        return "dispatch_mach_port_kernel_rpc_trap";
    case Handler::Count:
        break;
    }
    return "invalid";
}
std::string_view contract_name(Contract value)
{
    switch (value) {
    case Contract::CurrentDispatcher:
        return "CurrentDispatcher";
    case Contract::LaterEpoch:
        return "LaterEpoch";
    case Contract::ResourceCoalitions:
        return "ResourceCoalitions";
    case Contract::Ledger:
        return "Ledger";
    case Contract::IoPolicy:
        return "IoPolicy";
    case Contract::GuardedFdChange:
        return "GuardedFdChange";
    case Contract::Connectx:
        return "Connectx";
    case Contract::StackSnapshot:
        return "StackSnapshot";
    case Contract::SemaphoreValue:
        return "SemaphoreValue";
    case Contract::NamedSysctl:
        return "NamedSysctl";
    case Contract::Psynch:
        return "Psynch";
    case Contract::LegacySharedRegion:
        return "LegacySharedRegion";
    case Contract::SharedRegionSlide:
        return "SharedRegionSlide";
    case Contract::PthreadRegisterV1:
        return "PthreadRegisterV1";
    case Contract::PthreadIdentity:
        return "PthreadIdentity";
    case Contract::PthreadControl:
        return "PthreadControl";
    case Contract::MachDirectRpc:
        return "MachDirectRpc";
    case Contract::MachMixedVm:
        return "MachMixedVm";
    case Contract::MachWideVm:
        return "MachWideVm";
    case Contract::MachLegacyInit:
        return "MachLegacyInit";
    case Contract::Count:
        break;
    }
    return "invalid";
}
std::string_view domain_name(Domain value)
{
    switch (value) {
    case Domain::BsdSyscall:
        return "bsd";
    case Domain::MachTrap:
        return "mach";
    case Domain::ArmFastTrap:
        return "arm-fast";
    case Domain::MigRoutine:
        return "mig";
    }
    return "invalid";
}
std::string_view outcome_name(Outcome value)
{
    switch (value) {
    case Outcome::HandlerValidated:
        return "handler-validates";
    case Outcome::BsdNosys:
        return "nosys-policy";
    case Outcome::BsdUnknown:
        return "trace-unknown+nosys-policy";
    case Outcome::MachUnknown:
        return "trace-unknown+invalid-argument";
    case Outcome::MigFallback:
        return "send-invalid-destination/MIG-fallback";
    }
    return "invalid";
}
}
