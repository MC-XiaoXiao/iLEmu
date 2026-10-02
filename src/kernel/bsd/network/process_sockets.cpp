// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Apply process-wide socket shutdown requests to owned endpoints.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/bsd/vm/vm_unix.c

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"

#include "../support.hpp"

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_process_sockets(Cpu& cpu)
{
    const auto target_pid = cpu.registers()[0];
    const auto level = cpu.registers()[1];
    // xnu bsd/vm/vm_unix.c accepts service (1) or all (2).
    // Per-socket defunct errors do not fail the process-level operation.
    if (level != 1U && level != 2U) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
    }
    {
        std::lock_guard lock { shared_state_->mach_mutex };
        const auto target = shared_state_->processes.find(target_pid);
        if (target == shared_state_->processes.end() || target->second.exited ||
            (process_.effective_uid != 0U && target_pid != process_.pid &&
                (target->second.uid != process_.effective_uid ||
                    target->second.effective_uid != process_.effective_uid ||
                    target->second.gid != process_.gid))) {
            bsd_error(cpu, darwin::error::operation_not_permitted);
            return;
        }
    }
    if (target_pid == process_.pid)
        shutdown_process_sockets(level);
    else if (process_sockets_shutdown_handler_)
        process_sockets_shutdown_handler_(target_pid, level);
    else {
        bsd_error(cpu, bsd_support::not_implemented);
        return;
    }
    output_.write("[network] pid-shutdown-sockets caller=" +
                  std::to_string(process_.pid) +
                  " target=" + std::to_string(target_pid) +
                  " level=" + std::to_string(level) + "\n");
    bsd_success(cpu, 0U);
}

void CompatibilityKernel::shutdown_process_sockets(std::uint32_t level)
{
    for (const auto& [fd, descriptor] : virtual_descriptors_) {
        const auto local =
            descriptor.starts_with("unix-") || descriptor == "socketpair";
        if (level == 1U && !local)
            continue;
        // AF_UNIX defaults to SOF_NODEFUNCT. Native IPC clients opt in through
        // SO_DEFUNCTOK; do not tear down launchd or other protected IPC pairs.
        const auto options = socket_options_.find(fd);
        if (options == socket_options_.end() ||
            !options->second->begin_defunct(!local, [&] {
                // Preserve a pending transport error before host shutdown can
                // change it. Ineligible/already-defunct sockets never query it.
                if (const auto host = host_sockets_.find(fd);
                    host != host_sockets_.end()) {
                    const auto pending = host->second->socket_error();
                    // Preserve completed peers before publishing terminal state,
                    // so every alias observes a ready-to-consume accept queue.
                    static_cast<void>(host->second->shutdown(
                        darwin::socket::shutdown_read_write));
                    if (pending.status == HostSocketStatus::Success)
                        return pending.darwin_error;
                }
                return 0U;
            }))
            continue;
        if (const auto udp = virtual_udp_sockets_.find(fd);
            udp != virtual_udp_sockets_.end()) {
            udp->second->make_defunct();
        } else if (const auto stream = isolated_stream_sockets_.find(fd);
            stream != isolated_stream_sockets_.end()) {
            stream->second->make_defunct();
        } else if (const auto endpoint = socket_pair_endpoints_.find(fd);
            endpoint != socket_pair_endpoints_.end()) {
            std::lock_guard lock { shared_state_->socket_mutex };
            endpoint->second.shutdown_read();
            endpoint->second.shutdown_write();
        }
    }
    shared_state_->note_io_event_transition();
}

} // namespace ilemu
