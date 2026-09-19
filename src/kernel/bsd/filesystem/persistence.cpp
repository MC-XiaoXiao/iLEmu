// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Handle guest file synchronization and persistence requests.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/bsd/vfs/vfs_syscalls.c

#include "kernel/kernel.hpp"

#include "foundation/host_file_sync.hpp"

#include "kernel/darwin_abi.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include "../support.hpp"

namespace ilemu {

bool CompatibilityKernel::deliver_pending_file_sync(Cpu& cpu)
{
    if (const auto pending = pending_file_syncs_.find(cpu.processor_id());
        pending != pending_file_syncs_.end()) {
        const auto result = pending->second.request->result();
        if (!result)
            return false;
        if (*result != 0) {
            bsd_error(cpu, bsd_support::darwin_filesystem_error(
                std::error_code { *result, std::generic_category() }));
        } else {
            bsd_success(cpu, 0);
            output_.write("[vfs] fsync fd=" +
                          std::to_string(pending->second.fd) + "\n");
        }
        pending_file_syncs_.erase(pending);
        process_.waiting_for_events = false;
        cpu.clear_halt();
        return true;
    }
    return false;
}

bool CompatibilityKernel::dispatch_bsd_filesystem_persistence(
    Cpu& cpu, std::uint32_t number)
{
    if (number != darwin::syscall::synchronize_file)
        return false;

    auto fd = cpu.registers()[0];
    if (const auto duplicate = duplicated_descriptors_.find(fd);
        duplicate != duplicated_descriptors_.end()) {
        fd = duplicate->second;
    }
    const auto descriptor = file_descriptors_.find(fd);
    if (descriptor == file_descriptors_.end()) {
        bsd_error(cpu, bsd_support::bad_file_descriptor);
        return true;
    }

    const auto description = ensure_regular_file_open_description(fd);
    if (!description) {
        bsd_error(cpu, bsd_support::bad_file_descriptor);
        return true;
    }
    std::shared_ptr<HostFileSynchronizer> synchronizer;
    {
        const std::lock_guard lock { shared_state_->filesystem_mutex };
        if (!shared_state_->file_synchronizer) {
            shared_state_->file_synchronizer =
                std::make_shared<HostFileSynchronizer>();
        }
        synchronizer = shared_state_->file_synchronizer;
    }
    auto request = synchronizer->synchronize(description->host_descriptor());
    pending_file_syncs_.insert_or_assign(cpu.processor_id(),
        PendingFileSync { fd, std::move(request) });
    process_.waiting_for_events = true;
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
    return true;
}

} // namespace ilemu
