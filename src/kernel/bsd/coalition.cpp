// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Implement the Darwin resource-coalition lifecycle and BSD syscall ABI.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.1.97/bsd/kern/sys_coalition.c

#include "kernel/darwin_coalition_runtime.hpp"

#include "device_state/darwin_abi.hpp"
#include "foundation/darwin_errno.hpp"
#include "kernel/kernel.hpp"

#include "support.hpp"

#include <limits>
#include <utility>

namespace ilemu {
namespace {

    constexpr std::uint32_t create_operation = 1;
    constexpr std::uint32_t terminate_operation = 2;
    constexpr std::uint32_t reap_operation = 3;
    constexpr std::uint32_t create_privileged_flag = 1;
    constexpr std::uint64_t default_coalition_identifier = 1;

} // namespace

DarwinCoalitionRuntime::CreateResult
DarwinCoalitionRuntime::create(std::uint32_t flags)
{
    if ((flags & ~create_privileged_flag) != 0)
        return { bsd_support::invalid_argument, 0 };
    const std::lock_guard lock { mutex_ };
    if (next_identifier_ == std::numeric_limits<std::uint64_t>::max())
        return { darwin::error::no_memory, 0 };
    const auto identifier = next_identifier_++;
    coalitions_.emplace(identifier,
        Coalition { .privileged = (flags & create_privileged_flag) != 0 });
    return { 0, identifier };
}

std::uint32_t DarwinCoalitionRuntime::request_terminate(
    std::uint64_t identifier, std::uint32_t flags)
{
    if (flags != 0)
        return bsd_support::invalid_argument;
    if (identifier == default_coalition_identifier)
        return darwin::error::operation_not_permitted;
    const std::lock_guard lock { mutex_ };
    const auto found = coalitions_.find(identifier);
    if (found == coalitions_.end())
        return darwin::error::no_such_process;
    if (found->second.terminated)
        return darwin::error::already_in_progress;
    found->second.terminated = true;
    return 0;
}

std::uint32_t DarwinCoalitionRuntime::reap(
    std::uint64_t identifier, std::uint32_t flags)
{
    if (flags != 0)
        return bsd_support::invalid_argument;
    if (identifier == default_coalition_identifier)
        return darwin::error::operation_not_permitted;
    const std::lock_guard lock { mutex_ };
    const auto found = coalitions_.find(identifier);
    if (found == coalitions_.end())
        return darwin::error::no_such_process;
    if (!found->second.terminated || found->second.active_count != 0)
        return darwin::error::device_busy;
    coalitions_.erase(found);
    return 0;
}

void CompatibilityKernel::dispatch_bsd_coalition(Cpu& cpu)
{
    const auto& registers = cpu.registers();
    const auto operation = registers[0];
    const auto identifier_address = registers[1];
    const auto flags = registers[2];
    if (identifier_address == 0 ||
        identifier_address >
            std::numeric_limits<std::uint32_t>::max() - sizeof(std::uint64_t) + 1U) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }

    auto& runtime = shared_state_->coalitions;
    if (operation == create_operation) {
        const auto result = runtime.create(flags);
        if (result.error != 0) {
            bsd_error(cpu, result.error);
        } else if (!memory_.write64(identifier_address, result.identifier)) {
            bsd_error(cpu, bsd_support::bad_address);
        } else {
            bsd_success(cpu, 0);
        }
        return;
    }

    const auto identifier = memory_.read64(identifier_address);
    if (!identifier) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    const auto error = operation == terminate_operation
                           ? runtime.request_terminate(*identifier, flags)
                           : operation == reap_operation
                           ? runtime.reap(*identifier, flags)
                           : bsd_support::not_implemented;
    if (error != 0)
        bsd_error(cpu, error);
    else
        bsd_success(cpu, 0);
}

} // namespace ilemu
