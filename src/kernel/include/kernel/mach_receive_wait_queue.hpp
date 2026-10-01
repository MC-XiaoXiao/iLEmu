// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ilemu {

// Native receivers leave the wait queue at post time, before their thread runs.
// Selected messages survive suspension independently of the port queue.
// All access holds mach_mutex; polling uses the existing object generation.
template <class Message, class Receive> class MachReceiveWaitQueue {
public:
    enum class State { Waiting, MessageReady, TooLarge, TimedOut, PortChanged };
    struct Ticket {
        std::uint32_t task;
        Receive receive;
        State state { State::Waiting };
        std::optional<Message> message;
        std::uint32_t destination { };
        std::uint32_t sequence_number { };
        std::uint32_t message_size { };
    };
    void wait(std::uint32_t task, const Receive& receive)
    {
        auto [it, inserted] = tickets_.try_emplace(
            receive.wait_queue_sequence, Ticket { task, receive });
        if (inserted)
            waiting_[*receive.receive_object].emplace(
                receive.wait_queue_sequence, &it->second);
    }
    Ticket* find(std::uint64_t sequence)
    {
        const auto it = tickets_.find(sequence);
        return it == tickets_.end() ? nullptr : &it->second;
    }
    Ticket* first(std::uint32_t object)
    {
        const auto it = waiting_.find(object);
        return it == waiting_.end() ? nullptr : it->second.begin()->second;
    }
    bool empty() const { return waiting_.empty(); }
    void complete(Ticket& ticket, State state)
    {
        detach(ticket);
        ticket.state = state;
    }
    void retire(std::uint64_t sequence)
    {
        const auto it = tickets_.find(sequence);
        if (it == tickets_.end())
            return;
        detach(it->second);
        tickets_.erase(it);
    }
    template <class Discard>
    void cancel(std::uint32_t task, std::optional<std::uint32_t> processor,
        Discard discard)
    {
        // Rights destruction can reenter IPC. Detach first, then destroy.
        std::vector<Message> messages;
        for (auto it = tickets_.begin(); it != tickets_.end();) {
            auto& ticket = it->second;
            if (ticket.task != task ||
                (processor && ticket.receive.processor != *processor)) {
                ++it;
                continue;
            }
            detach(ticket);
            if (ticket.message)
                messages.push_back(std::move(*ticket.message));
            it = tickets_.erase(it);
        }
        for (auto& message : messages)
            discard(message);
    }

private:
    void detach(const Ticket& ticket)
    {
        const auto it = waiting_.find(*ticket.receive.receive_object);
        if (it == waiting_.end())
            return;
        it->second.erase(ticket.receive.wait_queue_sequence);
        if (it->second.empty())
            waiting_.erase(it);
    }
    std::map<std::uint64_t, Ticket> tickets_;
    std::map<std::uint32_t, std::map<std::uint64_t, Ticket*>> waiting_;
};
} // namespace ilemu
