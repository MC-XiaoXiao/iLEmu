// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "kernel/kernel_shared_state.hpp"
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace ilemu::kernel_bsd {
// XNU kern_proc.c fixjobc/orphanpg: only a transition from a qualified
// group to an orphaned group signals stopped jobs. Snapshot around the
// whole mutation, matching enterpgrp's increment-before-decrement ordering.
// Callers hold mach_mutex; signal delivery happens after releasing it.
class JobControlTransition {
public:
    explicit JobControlTransition(const KernelSharedState& state)
    {
        if (std::any_of(state.processes.begin(), state.processes.end(),
                [](const auto& entry) {
                    return !entry.second.exited && entry.second.signal_stopped;
                }))
            qualified_ = qualified(state);
    }

    std::vector<std::uint32_t> orphaned(const KernelSharedState& state) const
    {
        if (qualified_.empty()) return { };
        const auto remaining = qualified(state);
        std::unordered_set<std::uint32_t> stopped;
        for (const auto& [pid, member] : state.processes) {
            static_cast<void>(pid);
            const auto group = member.membership->group();
            if (!member.exited && member.signal_stopped &&
                qualified_.contains(group) && !remaining.contains(group))
                stopped.insert(group);
        }
        std::vector<std::uint32_t> targets;
        for (const auto& [pid, member] : state.processes)
            if (!member.exited && stopped.contains(member.membership->group()))
                targets.push_back(pid);
        return targets;
    }

private:
    static std::unordered_set<std::uint32_t> qualified(
        const KernelSharedState& state)
    {
        std::unordered_set<std::uint32_t> result;
        for (const auto& [pid, member] : state.processes) {
            static_cast<void>(pid);
            if (member.exited) continue;
            const auto parent = state.processes.find(member.parent_pid);
            if (parent == state.processes.end() || parent->second.exited) continue;
            const auto& current = *member.membership;
            const auto& ancestor = *parent->second.membership;
            if (current.group() != ancestor.group() && current.session == ancestor.session)
                result.insert(current.group());
        }
        return result;
    }
    std::unordered_set<std::uint32_t> qualified_;
};
} // namespace ilemu::kernel_bsd
