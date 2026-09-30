// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>

namespace ilemu {
class Cpu;
struct KernelSharedState;
struct ProcessContext;

// A kernel exception RPC is a thread continuation, not a guest syscall.
// Reuse the ordinary IPC queues/capabilities without a guest receive buffer.
class MachExceptionDelivery {
public:
    struct Exception {
        std::uint32_t type { };
        std::array<std::uint64_t, 2> codes { };
        std::array<std::uint32_t, 3> arm_state { };
        std::uint32_t signal { };
    };
    enum class Outcome { Waiting, Handled, Unhandled };
    struct Completion {
        Outcome outcome;
        Exception exception;
    };
    Completion begin(
        KernelSharedState&, const ProcessContext&, Cpu&, Exception);
    Completion poll(KernelSharedState&, const ProcessContext&, Cpu&);
    [[nodiscard]] bool waiting(std::size_t slot) const
    {
        return pending_.contains(slot);
    }
    [[nodiscard]] bool ready(KernelSharedState&, std::size_t slot) const;
    void cancel(KernelSharedState&, std::size_t slot);
    void clear(KernelSharedState&);

private:
    struct Pending {
        Exception exception;
        std::uint32_t next_level { };
        std::uint32_t reply_object { };
        std::uint32_t reply_identifier { };
        std::uint32_t behavior { };
    };
    bool advance_locked(
        KernelSharedState&, const ProcessContext&, Cpu&, Pending&);
    void retire_reply_locked(KernelSharedState&, Pending&);
    std::map<std::size_t, Pending> pending_;
};
} // namespace ilemu
