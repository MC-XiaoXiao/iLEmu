// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "kernel/mach_arm_thread_abi.hpp"
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace ilemu::darwin::signal_context {

// ARM ILP32: bsd/sys/ucontext.h, bsd/arm/_mcontext.h and
// mach/arm/_structs.h. No host pointers or host floating-point types.
struct UserContext {
    std::uint32_t on_stack;
    std::uint32_t signal_mask;
    std::uint32_t stack_address;
    std::uint32_t stack_size;
    std::uint32_t stack_flags;
    std::uint32_t link;
    std::uint32_t machine_size;
    std::uint32_t machine_address;
};

struct MachineContext {
    std::array<std::uint32_t, 3> exception;
    arm_thread::GeneralState general;
    std::array<std::uint32_t, 64> floating;
    std::uint32_t fpscr;
};

// sys/signal.h user32_siginfo_t. Firmware trampolines take the context
// pointer from entry SP; the remaining objects are reached by pointers.
// Target ARM32 trampolines predate the later sigreturn validation token.
struct SignalInfo {
    std::uint32_t signal, error, code, pid, uid, status, address, value, band;
    std::array<std::uint32_t, 7> padding;
};
struct Frame {
    std::uint32_t context_address;
    SignalInfo information;
    UserContext user;
    MachineContext machine;
};
inline constexpr std::uint32_t on_stack_action = 0x01U;
inline constexpr std::uint32_t reset_action = 0x04U;
inline constexpr std::uint32_t no_defer_action = 0x10U;
inline constexpr std::uint32_t information_action = 0x40U;

// The firmware's __sigtramp uses 184/30; __sigunaltstack uses these
// flag-only operations (no ucontext copyin). The obsolete slot 103 is not
// the ARM sigreturn entry.
inline constexpr std::uint32_t syscall = 184;
inline constexpr std::uint32_t flavor = 30;
inline constexpr std::uint32_t set_alternate_stack = 0x40000000U;
inline constexpr std::uint32_t reset_alternate_stack = 0x80000000U;

// arm/status.c preserves privileged CPSR bits during thread_setstatus;
// unix_signal.c additionally forces user mode before restoring the state.
constexpr std::uint32_t restored_cpsr(std::uint32_t requested,
    std::uint32_t current)
{
    return (arm_thread::restored_cpsr(requested, current) & ~0x1fU) |
           arm_thread::user_cpsr;
}

static_assert(std::endian::native == std::endian::little);
static_assert(sizeof(UserContext) == 32);
static_assert(sizeof(SignalInfo) == 64);
static_assert(offsetof(SignalInfo, padding) == 36);
static_assert(sizeof(Frame) == 440);
static_assert(offsetof(Frame, information) == 4);
static_assert(offsetof(Frame, user) == 68);
static_assert(offsetof(Frame, machine) == 100);
static_assert(offsetof(UserContext, machine_size) == 24);
static_assert(offsetof(UserContext, machine_address) == 28);
static_assert(sizeof(MachineContext) == 340);
static_assert(offsetof(MachineContext, general) == 12);
static_assert(offsetof(MachineContext, floating) == 80);
static_assert(offsetof(MachineContext, fpscr) == 336);

} // namespace ilemu::darwin::signal_context
