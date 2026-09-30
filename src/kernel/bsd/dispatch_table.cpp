// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/bsd_dispatch_table.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel.hpp"
#include "kernel/xnu_reference_syscalls.hpp"
#include "process/resource_monitor.hpp"
#include "support.hpp"

#include <limits>
#include <stdexcept>

namespace ilemu {
BsdDispatchTable::Adapter BsdDispatchTable::adapter_for(
    syscall_routes::Handler handler)
{
    using syscall_routes::Handler;
    switch (handler) {
    case Handler::BsdProcess:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_process(cpu, number));
        };
    case Handler::BsdProcessSockets:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t) {
            static_cast<void>(kernel.dispatch_bsd_process_sockets(cpu));
        };
    case Handler::BsdPosixSemaphore:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_posix_semaphore(cpu, number));
        };
    case Handler::BsdSignal:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_signal(cpu, number));
        };
    case Handler::BsdPlatform:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_platform(cpu, number));
        };
    case Handler::BsdFilesystem:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_filesystem(cpu, number));
        };
    case Handler::BsdDescriptorMemory:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(
                kernel.dispatch_bsd_descriptor_memory(cpu, number));
        };
    case Handler::BsdSharedRegion:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_shared_region(cpu, number));
        };
    case Handler::BsdPsynch:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_psynch(cpu, number));
        };
    case Handler::BsdAio:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_aio(cpu, number));
        };
    case Handler::BsdDebug:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_debug(cpu, number));
        };
    case Handler::BsdSocket:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_socket(cpu, number));
        };
    case Handler::BsdEvents:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_events(cpu, number));
        };
    case Handler::BsdKqueue:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_kqueue(cpu, number));
        };
    case Handler::BsdSecurity:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_security(cpu, number));
        };
    case Handler::BsdAuditSession:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_audit_session(cpu, number));
        };
    case Handler::BsdFileport:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_fileport(cpu, number));
        };
    case Handler::BsdGuardedFile:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_guarded_file(cpu, number));
        };
    case Handler::BsdCoalition:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t) {
            static_cast<void>(kernel.dispatch_bsd_coalition(cpu));
        };
    case Handler::BsdNetworkPolicy:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t) {
            static_cast<void>(kernel.dispatch_bsd_network_policy(cpu));
        };
    case Handler::BsdDirectoryAttributes:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t) {
            static_cast<void>(kernel.dispatch_bsd_directory_attributes(cpu));
        };
    case Handler::BsdPthread:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            static_cast<void>(kernel.dispatch_bsd_pthread(cpu, number));
        };
    case Handler::BsdCodeSigning:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) {
            kernel.dispatch_bsd_code_signing(
                cpu, number == darwin::syscall::code_signing_audit_operations);
        };
    case Handler::BsdLedgerInline:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t) {
            const auto& registers = cpu.registers();
            kernel.bsd_error(
                cpu, kernel_bsd::resource_monitor::query_ledger(kernel.memory_,
                         *kernel.shared_state_, registers[0], registers[1],
                         registers[2], registers[3]));
        };
    case Handler::BsdIoPolicyInline:
        return io_policy;
    case Handler::BsdStackSnapshotInline:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t) {
            // Retain the privileged diagnostic failure contract.
            kernel.bsd_error(cpu, darwin::error::permission_denied);
        };
    default:
        throw std::logic_error("missing BSD handler adapter");
    }
}

void BsdDispatchTable::io_policy(
    CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t)
{
    constexpr std::uint32_t iopol_cmd_get = 1;
    constexpr std::uint32_t iopol_cmd_set = 2;
    constexpr std::uint32_t iopol_type_disk = 0;
    constexpr std::uint32_t iopol_scope_process = 0;
    constexpr std::uint32_t iopol_scope_thread = 1;
    // The four-policy disk ABI defines DEFAULT/NORMAL/PASSIVE/THROTTLE
    // (0..3). Extended disk-policy ABIs add values and additional iotypes;
    // keep those newer values unimplemented instead of accidentally
    // applying the old policy state to a different ABI.
    constexpr std::uint32_t iopol_policy_max = 3;
    constexpr std::uint32_t iopol_policy_offset = 2U * sizeof(std::uint32_t);

    const auto address = cpu.registers()[1];
    if (address >
        std::numeric_limits<std::uint32_t>::max() - iopol_policy_offset) {
        kernel.bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    const auto scope = kernel.memory_.read32(address);
    const auto iotype = kernel.memory_.read32(address + sizeof(std::uint32_t));
    const auto policy =
        kernel.memory_.read32(address + 2U * sizeof(std::uint32_t));
    if (!scope || !iotype || !policy) {
        kernel.bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    if (*iotype != iopol_type_disk ||
        (*scope != iopol_scope_process && *scope != iopol_scope_thread)) {
        kernel.bsd_error(cpu, bsd_support::invalid_argument);
        return;
    }

    auto* stored_policy = &kernel.process_.disk_io_policy;
    if (*scope == iopol_scope_thread) {
        const auto thread_object =
            kernel.thread_object_for_processor(cpu.processor_id());
        if (!thread_object) {
            kernel.bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        stored_policy =
            &kernel.process_.thread_disk_io_policies[*thread_object];
    }
    switch (cpu.registers()[0]) {
    case iopol_cmd_get:
        if (!kernel.memory_.write32(
                address + 2U * sizeof(std::uint32_t), *stored_policy)) {
            kernel.bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        kernel.bsd_success(cpu, 0);
        return;
    case iopol_cmd_set:
        if (*policy > iopol_policy_max) {
            kernel.bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        *stored_policy = *policy;
        kernel.bsd_success(cpu, 0);
        return;
    default:
        kernel.bsd_error(cpu, bsd_support::invalid_argument);
        return;
    }
}

BsdDispatchTable::BsdDispatchTable(
    const DarwinAbi& abi, std::string_view darwin_release)
    : default_sigsys_ { abi.capabilities.send_sigsys }
{
    const auto table = syscall_routes::build(abi);
    for (std::uint32_t number = 0; number < bindings_.size(); ++number) {
        const auto* entry =
            table.find(syscall_routes::Domain::BsdSyscall, number);
        auto& binding = bindings_[number];
        const auto canonical = entry ? entry->canonical_number : number;
        binding.canonical = static_cast<std::uint16_t>(canonical);
        binding.send_sigsys =
            default_sigsys_ &&
            !xnu_reference::bsd_slot_defined(darwin_release, canonical);
        if (!entry)
            continue;
        binding.trace_unknown =
            entry->outcome == syscall_routes::Outcome::BsdUnknown;
        // Retain audited collisions and pthread precedence over the
        // conservative public-reference epoch gate.
        const bool priority_contract =
            entry->handler == syscall_routes::Handler::BsdPthread ||
            entry->contract == syscall_routes::Contract::SemaphoreValue ||
            entry->contract == syscall_routes::Contract::NamedSysctl ||
            canonical == 299U || canonical == 300U;
        if (entry->outcome == syscall_routes::Outcome::HandlerValidated &&
            (priority_contract ||
                !xnu_reference::bsd_slot_gated(darwin_release, canonical)))
            binding.adapter = adapter_for(entry->handler);
    }
}

bool BsdDispatchTable::sends_sigsys(std::uint32_t number) const
{
    return number < bindings_.size() ? bindings_[number].send_sigsys
                                     : default_sigsys_;
}

void BsdDispatchTable::dispatch(
    CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t number) const
{
    if (number < bindings_.size()) {
        const auto& binding = bindings_[number];
        if (binding.adapter) {
            binding.adapter(kernel, cpu, binding.canonical);
            return;
        }
        if (!binding.trace_unknown) {
            kernel.dispatch_bsd_nosys(cpu, binding.send_sigsys);
            return;
        }
    }
    kernel.output_.marker(
        "[process] unsupported-bsd pid=" + std::to_string(kernel.process_.pid) +
        " number=" + std::to_string(number));
    kernel.trace_unknown(cpu, "BSD syscall", number);
    kernel.dispatch_bsd_nosys(cpu, sends_sigsys(number));
}
} // namespace ilemu
