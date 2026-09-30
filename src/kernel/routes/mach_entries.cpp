// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "catalog_internal.hpp"
#include "kernel/mach_clock_abi.hpp"
#include "kernel/mach_scheduler_abi.hpp"
namespace ilemu::syscall_routes {
void bind_mach_entries(Table& table, const DarwinAbi& abi)
{
    const auto add = [&](std::uint32_t n, std::string_view name,
                         Handler handler = Handler::MachInline,
                         Contract contract = Contract::CurrentDispatcher,
                         Outcome outcome = Outcome::HandlerValidated,
                         std::string_view source = "mach/traps.cpp") {
        table.bind_new({ Domain::MachTrap, n, n, name, handler, contract,
            Cancellation::OriginalEntry, outcome, source });
    };
    add(3, "mach_absolute_time", Handler::MachClock);
    add(26, "mach_reply_port", Handler::MachTask);
    add(27, "thread_self_trap", Handler::MachThreadSelf);
    add(28, "task_self_trap", Handler::MachTask);
    add(29, "host_self_trap", Handler::MachTask);
    add(31, "mach_msg_trap", Handler::MachMessage);
    add(32, "mach_msg_overwrite_trap/r8-receive-buffer", Handler::MachMessage);
    add(33, "semaphore_signal_trap", Handler::MachSemaphore);
    add(34, "semaphore_signal_all_trap", Handler::MachSemaphore);
    add(35, "semaphore_signal_thread_trap", Handler::MachSemaphore);
    add(36, "semaphore_wait_trap", Handler::MachSemaphore);
    add(37, "semaphore_wait_signal_trap", Handler::MachSemaphore);
    add(38, "semaphore_timedwait_trap", Handler::MachSemaphore);
    add(39, "semaphore_timedwait_signal_trap", Handler::MachSemaphore);
    add(41, "init_process", Handler::MachInline, Contract::MachLegacyInit);
    add(44, "task_name_for_pid", Handler::MachTask);
    add(45, "task_for_pid", Handler::MachTask);
    add(46, "pid_for_task", Handler::MachTask);
    add(darwin::mach::scheduler::swtch_pri_trap, "swtch_pri", Handler::MachScheduler);
    add(darwin::mach::scheduler::swtch_trap, "swtch", Handler::MachScheduler);
    add(darwin::mach::scheduler::thread_switch_trap, "thread_switch", Handler::MachScheduler);
    add(darwin::mach::clock::sleep_trap, "clock_sleep_trap", Handler::MachClock);
    add(89, "mach_timebase_info_trap", Handler::MachClock);
    add(90, "mach_wait_until_trap", Handler::MachClock);
    add(91, "mk_timer_create_trap", Handler::MachTimer);
    add(92, "mk_timer_destroy_trap", Handler::MachTimer);
    add(93, "mk_timer_arm_trap", Handler::MachTimer);
    add(94, "mk_timer_cancel_trap", Handler::MachTimer);
    const bool direct =
        abi.mach_kernel_rpc != DarwinMachKernelRpcAbi::LegacyMigOnly;
    const bool wide =
        abi.mach_kernel_rpc == DarwinMachKernelRpcAbi::DirectWideVmAndPortTraps;
    const auto vm = [&](std::uint32_t n, std::string_view name) {
        // Wide table (xnu-2422+): 11/13 are kern_invalid (KERN_INVALID_ARGUMENT);
        // 15 is mach_vm_map, answered with the MIG fallback.
        const bool reserved = wide && (n == 11 || n == 13);
        const bool fallback = wide && n == 15;
        add(n, name, Handler::MachVmRpc,
            wide ? Contract::MachWideVm : Contract::MachMixedVm,
            reserved ? Outcome::MachInvalid
            : !direct ? Outcome::MachUnknown
            : fallback          ? Outcome::MigFallback
                                : Outcome::HandlerValidated,
            "mach/vm/kernel_rpc.cpp");
    };
    vm(10, "mach_vm_allocate");
    vm(11, wide ? "reserved-vm_allocate" : "vm_allocate");
    vm(12, "mach_vm_deallocate");
    vm(13, wide ? "reserved-vm_deallocate" : "vm_deallocate");
    vm(14, "mach_vm_protect");
    vm(15, wide ? "mach_vm_map" : "vm_protect");
    const auto port = [&](std::uint32_t n, std::string_view name,
                          bool guarded = false) {
        add(n, name, Handler::MachPortRpc,
            guarded ? Contract::MachWideVm : Contract::MachDirectRpc,
            (guarded ? wide : direct) ? Outcome::HandlerValidated
                                      : Outcome::MachUnknown,
            "mach/port/kernel_rpc.cpp");
    };
    port(16, "mach_port_allocate");
    port(17, "mach_port_destroy");
    port(18, "mach_port_deallocate");
    port(19, "mach_port_mod_refs");
    port(20, "mach_port_move_member");
    port(21, "mach_port_insert_right");
    port(22, "mach_port_insert_member");
    port(23, "mach_port_extract_member");
    port(24, "mach_port_construct", true);
    port(25, "mach_port_destruct", true);
    port(42, "mach_port_unguard", true);
    if (wide) {
        const Entry expected { Domain::MachTrap, 41, 41, "init_process",
            Handler::MachInline, Contract::MachLegacyInit,
            Cancellation::OriginalEntry, Outcome::HandlerValidated,
            "mach/traps.cpp" };
        auto entry = expected;
        entry.operation = "mach_port_guard";
        entry.handler = Handler::MachPortRpc;
        entry.contract = Contract::MachWideVm;
        entry.source = "mach/port/kernel_rpc.cpp";
        table.replace_entry(expected, entry);
    }
}
}
