// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ilemu {

// ipc_mqueue gives an awakened sender a slot before it runs. Keep that
// reservation separate from posted messages so a new sender cannot steal it.
// All mutation requires mach_mutex; readiness alone is read without the lock.
template <class Message> class MachSendWaitQueue {
public:
    enum class State { Waiting, Granted, TimedOut, Destroyed, Cancelled };
    struct Ticket {
        Message message;
        std::uint32_t task, processor, destination;
        std::optional<std::uint64_t> deadline;
        std::atomic<State> state { State::Waiting };
        Ticket(Message&& value, std::uint32_t pid, std::uint32_t cpu,
            std::uint32_t object, std::optional<std::uint64_t> due)
            : message(std::move(value))
            , task(pid)
            , processor(cpu)
            , destination(object)
            , deadline(due)
        {
        }
        bool ready(std::uint64_t now) const
        {
            return state.load(std::memory_order_acquire) != State::Waiting ||
                   (deadline && now >= *deadline);
        }
    };
    using Handle = std::shared_ptr<Ticket>;
    Handle wait(Message&& message, std::uint32_t task, std::uint32_t processor,
        std::uint32_t destination, std::optional<std::uint64_t> deadline)
    {
        auto ticket = std::make_shared<Ticket>(
            std::move(message), task, processor, destination, deadline);
        queues_[destination].tickets.push_back(ticket);
        return ticket;
    }
    std::size_t reserved(std::uint32_t object) const
    {
        if (queues_.empty())
            return 0;
        const auto found = queues_.find(object);
        return found == queues_.end() ? 0 : found->second.reserved;
    }
    bool empty() const { return queues_.empty(); }
    bool grant(std::uint32_t object, std::size_t count, std::uint64_t now)
    {
        const auto found = queues_.find(object);
        if (found == queues_.end())
            return false;
        bool changed = false;
        for (const auto& ticket : found->second.tickets) {
            if (ticket->state.load(std::memory_order_relaxed) != State::Waiting)
                continue;
            if (ticket->deadline && now >= *ticket->deadline) {
                ticket->state.store(State::TimedOut, std::memory_order_release);
                changed = true;
            } else if (count != 0) {
                --count;
                ++found->second.reserved;
                ticket->state.store(State::Granted, std::memory_order_release);
                changed = true;
            } else
                break;
        }
        return changed;
    }
    void retire(const Handle& ticket)
    {
        const auto found = queues_.find(ticket->destination);
        if (found == queues_.end())
            return;
        if (ticket->state.load(std::memory_order_relaxed) == State::Granted)
            --found->second.reserved;
        found->second.tickets.remove(ticket);
        if (found->second.tickets.empty())
            queues_.erase(found);
    }
    void destroy(std::uint32_t object, std::uint64_t now)
    {
        const auto found = queues_.find(object);
        if (found == queues_.end())
            return;
        for (const auto& ticket : found->second.tickets) {
            const auto state = ticket->state.load(std::memory_order_relaxed);
            if (state == State::Waiting || state == State::Granted)
                ticket->state.store(state == State::Waiting &&
                                            ticket->deadline &&
                                            now >= *ticket->deadline
                                        ? State::TimedOut
                                        : State::Destroyed,
                    std::memory_order_release);
        }
        // Keep ownership until the sender copies out or discards its message.
        found->second.reserved = 0;
    }
    template <class Discard>
    void cancel(std::uint32_t task, std::optional<std::uint32_t> processor,
        Discard discard)
    {
        std::vector<Handle> cancelled;
        for (auto queue = queues_.begin(); queue != queues_.end();) {
            for (auto it = queue->second.tickets.begin();
                it != queue->second.tickets.end();) {
                const auto& ticket = *it;
                if (ticket->task != task ||
                    (processor && ticket->processor != *processor)) {
                    ++it;
                    continue;
                }
                if (ticket->state.load(std::memory_order_relaxed) ==
                    State::Granted)
                    --queue->second.reserved;
                ticket->state.store(
                    State::Cancelled, std::memory_order_release);
                // Discard outside iteration: destroying a transferred receive
                // right may recursively mutate this queue collection.
                cancelled.push_back(ticket);
                it = queue->second.tickets.erase(it);
            }
            if (queue->second.tickets.empty())
                queue = queues_.erase(queue);
            else
                ++queue;
        }
        for (const auto& ticket : cancelled)
            discard(*ticket);
    }

private:
    struct Queue {
        std::list<Handle> tickets;
        std::size_t reserved { };
    };
    std::map<std::uint32_t, Queue> queues_;
};

} // namespace ilemu
