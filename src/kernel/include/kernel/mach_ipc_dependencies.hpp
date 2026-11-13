// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "mach/task_importance.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <vector>

namespace ilemu {

struct KernelSharedState;

// Observe queued calls without retaining a message, right or importance
// assertion. The existing assertion is disconnected on receive/cancellation.
// Native importance applies to async messages too; a header reply port is
// optional. When one exists, preserve its identity and ownership checks.
class QueuedMachDependencies {
public:
    struct Entry {
        std::uint32_t destination;
        std::uint32_t reply;
        std::weak_ptr<xnu::ipc::TaskImportance> sender_task;
        std::weak_ptr<xnu::ipc::ImportancePort> reply_port;
        std::weak_ptr<xnu::ipc::ImportanceAssertion> importance;
    };

    void track(std::uint32_t sender, std::uint32_t destination,
        std::uint32_t reply,
        const std::shared_ptr<xnu::ipc::TaskImportance>& sender_task,
        const std::shared_ptr<xnu::ipc::ImportancePort>& reply_port,
        const std::shared_ptr<xnu::ipc::ImportanceAssertion>& importance)
    {
        if (!importance || !sender_task || (reply != 0U && !reply_port))
            return;
        try {
            auto& entries = senders_[sender];
            std::erase_if(entries, [](const auto& entry) {
                const auto assertion = entry.importance.lock();
                return !assertion || !assertion->source();
            });
            for (const auto& entry : entries)
                if (entry.importance.lock() == importance)
                    return;
            // This bounds optional dispatch metadata, never the IPC queue itself.
            if (entries.size() == 256U)
                return;
            entries.push_back(
                { destination, reply, sender_task, reply_port, importance });
        } catch (const std::bad_alloc&) {
            // Optional host scheduling metadata must never reject a send.
        }
    }

    template<typename Visitor>
    void visit(std::uint32_t sender, Visitor&& visitor)
    {
        const auto found = senders_.find(sender);
        if (found == senders_.end())
            return;
        std::erase_if(found->second, [&](const auto& entry) {
            const auto assertion = entry.importance.lock();
            return !assertion || !assertion->source() ||
                   !visitor(entry, *assertion);
        });
        if (found->second.empty())
            senders_.erase(found);
    }

    void erase_sender(std::uint32_t sender) { senders_.erase(sender); }

private:
    std::map<std::uint32_t, std::vector<Entry>> senders_;
};

// Call under mach_mutex. Importance belongs to a task, including async RPCs
// sent by reusable workqueue workers. A queued assertion never identifies a
// particular thread's subsequent wait. All identities below are native objects.
std::vector<std::uint32_t> mach_ipc_dependency_providers_locked(
    KernelSharedState& state, std::uint32_t sender);

} // namespace ilemu
