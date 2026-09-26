// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "catalog_internal.hpp"
#include "kernel/darwin_abi.hpp"
#include <stdexcept>
namespace ilemu::syscall_routes {
void bind_alias_entries(Table& table)
{
    const auto alias = [&](std::uint32_t original, std::uint32_t canonical,
                           std::string_view name) {
        const auto* target = table.find(Domain::BsdSyscall, canonical);
        if (!target)
            throw std::invalid_argument("nocancel target missing");
        auto entry = *target;
        entry.number = original;
        entry.canonical_number = canonical;
        entry.operation = name;
        entry.cancellation = Cancellation::NoCancelAlias;
        entry.source = "bsd/dispatch.cpp:canonical_no_cancel_syscall";
        table.bind_new(entry);
    };
    // Existing dispatcher aliases are ungated; cancellation behavior is not
    // newly implemented.
    alias(396, darwin::syscall::read, "read_nocancel");
    alias(397, darwin::syscall::write, "write_nocancel");
    alias(398, darwin::syscall::open, "open_nocancel");
    alias(399, darwin::syscall::close, "close_nocancel");
    alias(400, 7U, "wait4_nocancel");
    alias(401, darwin::syscall::receive_message, "recvmsg_nocancel");
    alias(402, darwin::syscall::send_message, "sendmsg_nocancel");
    alias(403, darwin::syscall::receive_from, "recvfrom_nocancel");
    alias(404, darwin::syscall::accept, "accept_nocancel");
    alias(405, darwin::syscall::memory_synchronize, "msync_nocancel");
    alias(406, darwin::syscall::fcntl, "fcntl_nocancel");
    alias(407, darwin::syscall::select, "select_nocancel");
    alias(417, darwin::syscall::poll, "poll_nocancel");
    alias(420, darwin::syscall::posix_semaphore_wait, "sem_wait_nocancel");
    alias(408, darwin::syscall::synchronize_file, "fsync_nocancel");
    alias(409, darwin::syscall::connect, "connect_nocancel");
    alias(410, 111U, "sigsuspend_nocancel");
    alias(412, darwin::syscall::write_vector, "writev_nocancel");
    alias(413, darwin::syscall::send_to, "sendto_nocancel");
    alias(414, 153U, "pread_nocancel");
    alias(415, 154U, "pwrite_nocancel");
    alias(421, darwin::syscall::aio_suspend, "aio_suspend_nocancel");
    alias(423, darwin::syscall::semaphore_wait_signal,
        "__semwait_signal_nocancel");
}
}
