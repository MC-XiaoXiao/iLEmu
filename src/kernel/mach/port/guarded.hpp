// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <array>
#include <cstdint>

namespace ilemu {
class AddressSpace;
struct KernelSharedState;

// Shared IPC-space operations; callers hold mach_mutex and resolve a live task.
class GuardedPorts {
public:
    static constexpr std::uint32_t options_size = 24U;
    struct Options {
        std::uint32_t flags;
        std::uint32_t queue_limit;
    };
    struct Result {
        std::uint32_t error;
        std::uint32_t name { };
    };
    enum class Operation { Destruct, Guard, Unguard };
    static Result construct_locked(KernelSharedState& state, std::uint32_t task,
        Options options, std::uint64_t context);
    static std::uint32_t change_locked(KernelSharedState& state,
        std::uint32_t task, std::uint32_t name, Operation operation,
        std::uint64_t guard, std::int32_t delta = 0, bool strict = false);
};

// Caller holds mach_mutex. A zero task requests native remote-task fallback.
std::uint32_t dispatch_guarded_port_trap(KernelSharedState& state,
    AddressSpace& memory, std::uint32_t task,
    const std::array<std::uint32_t, 16>& registers, std::uint32_t trap);
}
