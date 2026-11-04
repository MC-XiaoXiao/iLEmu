// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/darwin_kqueue_abi.hpp"
#include "kernel/kevent_wire.hpp"
#include <algorithm>
#include <bit>

namespace ilemu {

void CompatibilityKernel::collect_host_socket_requests(std::uint32_t fd,
    std::uint8_t events, HostSocketRequests& requests,
    std::set<std::uint32_t>& visited) const
{
    if (const auto socket = host_sockets_.find(fd); socket != host_sockets_.end()) {
        auto& request = requests[fd];
        request.first = socket->second;
        request.second |= events;
        return;
    }
    if (!visited.insert(fd).second)
        return;
    if (const auto duplicate = duplicated_descriptors_.find(fd);
        duplicate != duplicated_descriptors_.end())
        collect_host_socket_requests(duplicate->second, events, requests, visited);
    if (const auto queue = kqueues_.find(fd); queue != kqueues_.end()) {
        for (const auto& registration : queue->second) {
            if (!registration.enabled)
                continue;
            if (registration.filter == darwin::kqueue::filter_read ||
                registration.filter == darwin::kqueue::filter_write)
                collect_host_socket_requests(static_cast<std::uint32_t>(registration.ident),
                    registration.filter == darwin::kqueue::filter_read
                        ? SocketReadable : SocketWritable, requests, visited);
        }
    }
}

CompatibilityKernel::HostSocketRequests CompatibilityKernel::host_readiness_requests(
    const std::array<std::uint32_t, 16>& registers, std::uint32_t number) const
{
    HostSocketRequests requests;
    std::set<std::uint32_t> visited;
    const auto add = [&](std::uint32_t fd, std::uint8_t events) {
        collect_host_socket_requests(fd, events, requests, visited);
    };
    if (number == darwin::syscall::poll && registers[1] <= 1024U) {
        for (std::uint32_t i = 0; i < registers[1]; ++i) {
            const auto address = registers[0] + i * darwin::poll::pollfd_size;
            const auto fd = memory_.read32(address);
            const auto events = memory_.read16(address + darwin::poll::events_offset);
            if (!fd || !events)
                return {};
            if (static_cast<std::int32_t>(*fd) < 0)
                continue;
            std::uint8_t mask = 0;
            if (*events & (darwin::poll::in | darwin::poll::read_normal | darwin::poll::read_band | darwin::poll::hangup))
                mask |= SocketReadable;
            if (*events & (darwin::poll::out | darwin::poll::write_normal | darwin::poll::write_band))
                mask |= SocketWritable;
            add(*fd, mask);
        }
    } else if (number == 93U && registers[0] <= 1024U) {
        for (std::uint32_t fd = 0; fd < registers[0]; ++fd) {
            std::uint8_t events = 0;
            for (const auto index : { 1U, 2U }) {
                if (registers[index] == 0)
                    continue;
                const auto bits = memory_.read32(registers[index] + fd / 32U * 4U);
                if (!bits)
                    return {};
                if (*bits & (1U << (fd % 32U)))
                    events |= index == 1 ? SocketReadable : SocketWritable;
            }
            if (events)
                add(fd, events);
        }
    } else if ((number == 363U || number == 369U || number == 374U) &&
               registers[2] <= 4096U && registers[4] != 0) {
        auto fd = registers[0];
        if (number == 374U) {
            const auto flags = memory_.read32(registers[13] + 28U);
            if (flags && (*flags & 0x20U))
                fd = darwin::kqueue::workqueue_descriptor;
        }
        add(fd, SocketReadable);
        const KeventWireFormat wire { number == 374U
            ? darwin::kqueue::WireFormat::QualityOfService
            : number == 369U ? darwin::kqueue::WireFormat::Extended64
                             : darwin::kqueue::WireFormat::Legacy32 };
        for (std::uint32_t i = 0; i < registers[2]; ++i) {
            const auto change = wire.read(memory_, registers[1] + i * wire.size());
            if (!change)
                return {};
            if (change->filter == darwin::kqueue::filter_read ||
                change->filter == darwin::kqueue::filter_write)
                add(static_cast<std::uint32_t>(change->ident),
                    change->filter == darwin::kqueue::filter_read
                        ? SocketReadable : SocketWritable);
        }
    }
    return requests;
}

void CompatibilityKernel::update_host_socket_watches(std::size_t processor,
    const HostSocketRequests& requests, bool snapshot)
{
    auto found = host_socket_watches_.find(processor);
    if (!snapshot && found != host_socket_watches_.end() &&
        found->second.size() == requests.size() &&
        std::equal(found->second.begin(), found->second.end(), requests.begin(),
            [](const auto& watch, const auto& request) {
                return watch.fd == request.first && watch.socket == request.second.first &&
                       watch.events == request.second.second;
            }))
        return;
    if (requests.empty()) {
        host_socket_watches_.erase(processor);
        return;
    }
    std::vector<HostSocketWatch> watches;
    for (const auto& [fd, request] : requests)
        watches.push_back({ fd, request.first, request.second,
            request.first->observe(request.second) });
    host_socket_watches_.insert_or_assign(processor, std::move(watches));
}

void CompatibilityKernel::refresh_host_socket_watches(std::size_t processor)
{
    if (pending_host_readiness_.contains(processor) ||
        (host_sockets_.empty() && !host_socket_watches_.contains(processor)))
        return;
    HostSocketRequests requests;
    std::set<std::uint32_t> visited;
    const auto add = [&](std::uint32_t fd, std::uint8_t events) {
        collect_host_socket_requests(fd, events, requests, visited);
    };
    if (const auto wait = pending_host_connects_.find(processor); wait != pending_host_connects_.end())
        add(wait->second.fd, SocketWritable);
    if (const auto wait = pending_host_accepts_.find(processor); wait != pending_host_accepts_.end())
        add(wait->second.fd, SocketReadable);
    if (const auto wait = pending_host_writes_.find(processor); wait != pending_host_writes_.end())
        requests[wait->second.fd] = { wait->second.socket, SocketWritable };
    if (const auto wait = pending_socket_reads_.find(processor); wait != pending_socket_reads_.end())
        add(wait->second.fd, SocketReadable);
    if (const auto wait = pending_recvmsgs_.find(processor); wait != pending_recvmsgs_.end())
        add(wait->second.fd, SocketReadable);
    if (const auto wait = pending_kevents_.find(processor); wait != pending_kevents_.end())
        add(wait->second.queue_fd, SocketReadable);
    if (const auto wait = pending_polls_.find(processor); wait != pending_polls_.end()) {
        for (const auto& entry : wait->second.entries) {
            if (entry.fd < 0)
                continue;
            std::uint8_t events = 0;
            if (entry.events & (darwin::poll::in | darwin::poll::read_normal | darwin::poll::read_band | darwin::poll::hangup))
                events |= SocketReadable;
            if (entry.events & (darwin::poll::out | darwin::poll::write_normal | darwin::poll::write_band))
                events |= SocketWritable;
            add(static_cast<std::uint32_t>(entry.fd), events);
        }
    }
    if (const auto wait = pending_selects_.find(processor); wait != pending_selects_.end()) {
        for (std::uint32_t fd = 0; fd < wait->second.descriptor_count; ++fd) {
            const auto bit = 1U << (fd % 32U);
            std::uint8_t events = 0;
            if (wait->second.read_words[fd / 32U] & bit) events |= SocketReadable;
            if (wait->second.write_words[fd / 32U] & bit) events |= SocketWritable;
            if (events) add(fd, events);
        }
    }
    update_host_socket_watches(processor, requests);
}

bool CompatibilityKernel::defer_host_readiness(Cpu& cpu, std::uint32_t number)
{
    if (host_sockets_.empty())
        return false;
    if (number != 93U && number != darwin::syscall::poll &&
        number != 363U && number != 369U && number != 374U)
        return false;
    const auto requests = host_readiness_requests(cpu.registers(), number);
    if (requests.empty())
        return false;
    // Positive levels survive until socket I/O advances the resource version.
    // Reuse them for repeated queries; a negative/unknown observation requires
    // a fresh asynchronous snapshot, including for a zero-timeout syscall.
    if (std::all_of(requests.begin(), requests.end(), [](const auto& request) {
            const auto events = request.second.second;
            const auto ready = request.second.first->readiness();
            return events != 0 && ((ready & events) == events ||
                (ready & (SocketDisconnected | SocketError)) != 0);
        }))
        return false;
    const auto processor = cpu.processor_id();
    update_host_socket_watches(processor, requests, true);
    pending_host_readiness_[processor] = { number, cpu.registers(), shared_state_->clock.now() };
    process_.waiting_for_events = true;
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
    return true;
}

std::optional<bool> CompatibilityKernel::deliver_host_readiness(Cpu& cpu)
{
    const auto processor = cpu.processor_id();
    const auto pending = pending_host_readiness_.find(processor);
    if (pending == pending_host_readiness_.end())
        return std::nullopt;
    // A close/dup2 while the snapshot was in flight must never let an old
    // resource's result describe a newly allocated socket with the same fd.
    update_host_socket_watches(processor,
        host_readiness_requests(pending->second.registers, pending->second.number));
    const auto watches = host_socket_watches_.find(processor);
    if (watches != host_socket_watches_.end() &&
        !std::all_of(watches->second.begin(), watches->second.end(),
            [](const auto& watch) { return watch.subscription->sampled(); }))
        return false;
    const auto invocation = pending->second;
    pending_host_readiness_.erase(pending);
    cpu.registers() = invocation.registers;
    cpu.clear_halt();
    process_.waiting_for_events = false;
    if (invocation.number == 93U || invocation.number == darwin::syscall::poll)
        dispatch_bsd_events(cpu, invocation.number);
    else
        dispatch_bsd_kqueue(cpu, invocation.number);
    const auto elapsed = shared_state_->clock.now() - invocation.guest_time;
    const auto preserve_deadline = [elapsed](auto& waits, std::size_t slot) {
        if (const auto wait = waits.find(slot); wait != waits.end() && wait->second.deadline)
            *wait->second.deadline -= std::min(*wait->second.deadline, elapsed);
    };
    preserve_deadline(pending_polls_, processor);
    preserve_deadline(pending_selects_, processor);
    preserve_deadline(pending_kevents_, processor);
    // Replacing the snapshot continuation can introduce a virtual deadline
    // without changing the processor's membership in the pending wait set.
    note_timer_deadline_transition();
    ++pending_event_topology_generation_;
    return !has_pending_event_locked(processor);
}

void CompatibilityKernel::close_host_socket_waits(std::uint32_t fd)
{
    const auto cancel = [&](const auto& waits) {
        for (const auto& [processor, wait] : waits) {
            if (wait.fd == fd) {
                closed_host_socket_waits_.insert(processor);
                host_socket_watches_.erase(processor);
            }
        }
    };
    cancel(pending_host_connects_);
    cancel(pending_host_accepts_);
    cancel(pending_socket_reads_);
    cancel(pending_recvmsgs_);
}

} // namespace ilemu
