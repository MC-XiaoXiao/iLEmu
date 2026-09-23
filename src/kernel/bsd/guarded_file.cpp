// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Guarded file descriptors reuse the normal VFS and descriptor lifecycle.
// ABI reference: XNU bsd/kern/kern_guarded.c, guarded_open_np/guarded_close_np.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "support.hpp"
#include <string>
namespace ilemu {
bool CompatibilityKernel::reject_guarded_descriptor(
    Cpu& cpu, std::uint32_t fd, std::uint32_t flags)
{
    const auto guard = descriptor_guards_.find(fd);
    if (guard == descriptor_guards_.end() || (guard->second.flags & flags) == 0U)
        return false;
    output_.line("[guard] fatal descriptor violation pid=" + std::to_string(process_.pid) +
        " fd=" + std::to_string(fd));
    // Preserve the fatal default disposition without performing the forbidden
    // operation. Mach EXC_GUARD exception-port delivery is not modeled yet.
    exit_process(0U, darwin::signal::kill);
    cpu.halt(Dynarmic::HaltReason::UserDefined1);
    return true;
}

void CompatibilityKernel::dispatch_bsd_guarded_file(Cpu& cpu, std::uint32_t number)
{
    auto& registers = cpu.registers();
    if (number == 444U) {
        const auto fd = registers[0];
        const auto old_guard_address = registers[1];
        const auto old_guard_flags = registers[2];
        const auto new_guard_address = registers[3];
        const auto new_guard_flags = registers[4];
        const auto descriptor_flags_address = registers[5];

        std::optional<std::uint64_t> old_guard;
        if (old_guard_address != 0U) {
            old_guard = memory_.read64(old_guard_address);
            if (!old_guard) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
        }
        std::optional<std::uint64_t> new_guard;
        if (new_guard_address != 0U) {
            new_guard = memory_.read64(new_guard_address);
            if (!new_guard) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
        }

        std::uint32_t requested_descriptor_flags = 0U;
        if (descriptor_flags_address != 0U) {
            const auto requested = memory_.read32(descriptor_flags_address);
            if (!requested) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            requested_descriptor_flags = *requested;
        }

        if (!descriptor_valid(fd)) {
            bsd_error(cpu, bsd_support::bad_file_descriptor);
            return;
        }

        const auto current_flags = descriptor_flags_.contains(fd)
                                       ? descriptor_flags_.at(fd)
                                       : 0U;
        if (descriptor_flags_address != 0U &&
            !memory_.write32(descriptor_flags_address, current_flags)) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }

        const auto found = descriptor_guards_.find(fd);
        const bool is_guarded = found != descriptor_guards_.end();
        if (is_guarded) {
            if (!old_guard || old_guard_flags == 0U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            if (*old_guard == 0U || *old_guard != found->second.identifier ||
                old_guard_flags != found->second.flags) {
                bsd_error(cpu, darwin::error::permission_denied);
                return;
            }
        } else if (old_guard || old_guard_flags != 0U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }

        if (new_guard) {
            if (*new_guard == 0U ||
                (new_guard_flags & DarwinFileGuard::duplicate) == 0U ||
                (new_guard_flags & ~DarwinFileGuard::supported_flags) != 0U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
        } else if (new_guard_flags != 0U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        } else if (!is_guarded) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }

        if (new_guard) {
            if (is_guarded) {
                auto updated_flags = current_flags;
                if ((found->second.flags & DarwinFileGuard::close) != 0U)
                    updated_flags &= ~DarwinFileGuard::descriptor_close_on_fork;
                if ((new_guard_flags & DarwinFileGuard::close) != 0U ||
                    (requested_descriptor_flags &
                        DarwinFileGuard::descriptor_close_on_fork) != 0U) {
                    updated_flags |= DarwinFileGuard::descriptor_close_on_fork;
                }
                found->second = DarwinFileGuard { *new_guard, new_guard_flags };
                descriptor_flags_[fd] = updated_flags;
            } else {
                descriptor_guards_.emplace(
                    fd, DarwinFileGuard { *new_guard, new_guard_flags });
                auto updated_flags =
                    current_flags | DarwinFileGuard::descriptor_close_on_exec;
                if ((new_guard_flags & DarwinFileGuard::close) != 0U)
                    updated_flags |= DarwinFileGuard::descriptor_close_on_fork;
                descriptor_flags_[fd] = updated_flags;
            }
        } else {
            descriptor_guards_.erase(found);
            constexpr auto descriptor_flag_mask =
                DarwinFileGuard::descriptor_close_on_exec |
                DarwinFileGuard::descriptor_close_on_fork;
            descriptor_flags_[fd] =
                (current_flags & ~descriptor_flag_mask) |
                (requested_descriptor_flags & descriptor_flag_mask);
        }
        bsd_success(cpu, 0U);
        return;
    }
    if (number == 443U) {
        const auto attributes = registers[1];
        if ((attributes & DarwinFileGuard::duplicate) == 0U ||
            (attributes & ~0x0fU) != 0U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        const auto guard = memory_.read64(registers[0]);
        if (!guard || *guard == 0U) {
            bsd_error(cpu, guard ? darwin::error::invalid_argument
                                : darwin::error::bad_address);
            return;
        }
        dispatch_bsd_kqueue(cpu, darwin::syscall::kqueue);
        if ((cpu.cpsr() & bsd_support::carry_flag) != 0U)
            return;
        const auto fd = registers[0];
        descriptor_guards_.emplace(fd, DarwinFileGuard { *guard, attributes });
        descriptor_flags_[fd] = DarwinFileGuard::descriptor_close_on_exec |
            ((attributes & DarwinFileGuard::close) != 0U
                    ? DarwinFileGuard::descriptor_close_on_fork
                    : 0U);
        output_.write("[guard] kqueue pid=" + std::to_string(process_.pid) +
            " fd=" + std::to_string(fd) + "\n");
        return;
    }
    const auto guard = memory_.read64(registers[1]);
    if (!guard) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    if (number == 441U) {
        const auto attributes = registers[2];
        const auto flags = registers[3];
        const auto mode = registers[4];
        constexpr std::uint32_t close_on_exec = 0x01000000U;
        if (*guard == 0U || (flags & close_on_exec) == 0U ||
            (attributes & DarwinFileGuard::duplicate) == 0U ||
            (attributes & ~0x0fU) != 0U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        registers[1] = flags;
        registers[2] = mode;
        dispatch_bsd_filesystem(cpu, 5U);
        if ((cpu.cpsr() & bsd_support::carry_flag) != 0U)
            return;
        const auto fd = registers[0];
        descriptor_guards_.emplace(fd, DarwinFileGuard { *guard, attributes });
        descriptor_flags_[fd] = DarwinFileGuard::descriptor_close_on_exec |
            ((attributes & DarwinFileGuard::close) != 0U
                    ? DarwinFileGuard::descriptor_close_on_fork
                    : 0U);
        output_.write("[guard] open pid=" + std::to_string(process_.pid) +
            " fd=" + std::to_string(fd) + "\n");
        return;
    }
    const auto fd = registers[0];
    const auto found = descriptor_guards_.find(fd);
    if (found == descriptor_guards_.end()) {
        bsd_error(cpu, export_descriptor(fd) ? darwin::error::invalid_argument
                                           : bsd_support::bad_file_descriptor);
        return;
    }
    if (found->second.identifier != *guard) {
        // A mismatched guarded close is fatal even if ordinary close is allowed.
        reject_guarded_descriptor(cpu, fd, DarwinFileGuard::duplicate);
        return;
    }
    if (release_file_descriptor(fd))
        bsd_success(cpu, 0U);
    else
        bsd_error(cpu, bsd_support::bad_file_descriptor);
}
} // namespace ilemu
