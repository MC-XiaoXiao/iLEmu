// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "resource_monitor.hpp"
#include "foundation/address_space.hpp"
#include "foundation/darwin_errno.hpp"
#include "kernel/kernel_shared_state.hpp"
#include <mutex>

namespace ilemu::kernel_bsd::resource_monitor {

std::uint32_t query_ledger(AddressSpace& memory, KernelSharedState& state,
    std::uint32_t command, std::uint32_t argument1,
    std::uint32_t argument2, std::uint32_t argument3)
{
    // XNU bsd/kern/sys_generic.c copies the signed entry count before
    // looking up the target. Template queries use the caller's template.
    constexpr std::uint32_t info = 0;
    constexpr std::uint32_t entry_info = 1;
    constexpr std::uint32_t template_info = 2;
    if (command == entry_info || command == template_info) {
        const auto count_address =
            command == entry_info ? argument3 : argument2;
        if (count_address == 0U ||
            !memory.accessible(count_address, 4U, MemoryPermission::Read))
            return darwin::error::bad_address;
        const auto count = memory.read32(count_address);
        if (!count)
            return darwin::error::bad_address;
        if ((*count & 0x8000'0000U) != 0U)
            return darwin::error::invalid_argument;
    }
    if (command != template_info) {
        std::lock_guard lock { state.mach_mutex };
        const auto target = state.processes.find(argument1);
        if (target == state.processes.end() || target->second.exited)
            return darwin::error::no_such_process;
    }
    // No task ledger is instantiated here. Match osfmk/kern/ledger.c's
    // NULL-ledger errors without fabricating balances or touching output.
    // Unknown commands and development-only LEDGER_LIMIT return EINVAL.
    return command == info ? darwin::error::no_entry
                           : darwin::error::invalid_argument;
}

std::uint32_t control(AddressSpace& memory, KernelSharedState& state,
    const ProcessContext& caller, std::uint32_t pid, std::uint32_t flavor,
    std::uint32_t argument)
{
    // XNU proc_rlimit_control uses an explicit PID, including for self.
    {
        std::lock_guard lock { state.mach_mutex };
        const auto target = state.processes.find(pid);
        if (target == state.processes.end() || target->second.exited)
            return darwin::error::no_such_process;
        if (pid != caller.pid && caller.effective_uid != 0U && caller.uid != 0U &&
            caller.effective_uid != target->second.effective_uid &&
            caller.uid != target->second.effective_uid)
            return darwin::error::permission_denied;
    }
    constexpr std::uint32_t wakeups_monitor = 1;
    constexpr std::uint32_t cpu_monitor = 2;
    constexpr std::uint32_t enable = 1;
    constexpr std::uint32_t disable = 2;
    constexpr std::uint32_t get_parameters = 4;
    constexpr std::uint32_t set_defaults = 8;
    constexpr std::uint32_t cpu_make_fatal = 0x1000;
    if (flavor == cpu_monitor)
        return (argument & cpu_make_fatal) != 0U
            ? darwin::error::not_supported : darwin::error::invalid_argument;
    if (flavor != wakeups_monitor)
        return darwin::error::invalid_argument;
    if (argument == 0U ||
        !memory.accessible(argument, 8U, MemoryPermission::Read))
        return darwin::error::bad_address;
    auto flags = *memory.read32(argument);
    auto rate = *memory.read32(argument + 4U);
    if ((flags & get_parameters) != 0U) {
        // No interrupt-wakeup ledger is active. GET takes precedence over
        // the other flags and reports the native disabled representation.
        flags = disable;
        rate = 0xffff'ffffU;
    } else if ((flags & enable) != 0U) {
        if ((flags & set_defaults) == 0U && static_cast<std::int32_t>(rate) < 0)
            return darwin::error::invalid_argument;
        // Do not claim enforcement when there is no guest accounting source.
        return darwin::error::not_supported;
    }
    if (!memory.accessible(argument, 8U, MemoryPermission::Write) ||
        !memory.write32(argument, flags) || !memory.write32(argument + 4U, rate))
        return darwin::error::bad_address;
    return 0;
}

} // namespace ilemu::kernel_bsd::resource_monitor
