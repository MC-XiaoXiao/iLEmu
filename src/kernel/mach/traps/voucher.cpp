// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// XNU direct Mach voucher trap ABI.
// Apple reference: xnu-3789.1.32/osfmk/ipc/mach_kernelrpc.c.

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_mach_task_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

#include "../support.hpp"

namespace ilemu {

void CompatibilityKernel::dispatch_mach_voucher_trap(
    Cpu& cpu, std::uint32_t trap)
{
    auto& registers = cpu.registers();
    if (trap == 43U) {
        // XNU's mach_generate_activity_id reserves up to sixteen IDs in the
        // system-wide voucher namespace and copies out the last one.
        const auto count = static_cast<std::int32_t>(registers[1]);
        if (count <= 0 || count > 16) {
            registers[0] = darwin::mach::invalid_argument;
            return;
        }
        std::uint64_t activity_id;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            activity_id = shared_state_->next_activity_id +
                static_cast<std::uint32_t>(count) - 1U;
            shared_state_->next_activity_id = activity_id + 1U;
        }
        registers[0] = memory_.write64(registers[2], activity_id)
            ? darwin::mach::success : darwin::mach::memory_error;
        return;
    }
    if (trap == 72U) {
        // XNU's direct trap takes a voucher name, attribute key, recipe
        // buffer, and in/out byte count. Reuse the same voucher state as the
        // Mach MIG extract path instead of synthesizing a separate recipe.
        constexpr std::uint32_t maximum_recipe_size = 5120U;
        constexpr std::uint32_t mig_array_too_large =
            static_cast<std::uint32_t>(-307);
        const auto capacity = memory_.read32(registers[3]);
        if (!capacity) {
            registers[0] = darwin::mach::memory_error;
            return;
        }
        if (*capacity > maximum_recipe_size) {
            registers[0] = mig_array_too_large;
            return;
        }

        std::vector<std::byte> recipe;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto object = mach_support::resolve_name_with_right(
                *shared_state_, process_.pid, registers[0],
                xnu::ipc::Right::Send);
            if (!object || !shared_state_->mach_vouchers.contains(*object)) {
                registers[0] =
                    darwin::mach_message::send_invalid_destination;
                return;
            }
            recipe = shared_state_->mach_vouchers.at(*object).extract(
                registers[1], true);
        }
        if (*capacity != 0 && !memory_.accessible(registers[2], *capacity,
                                   MemoryPermission::Read)) {
            registers[0] = darwin::mach::memory_error;
            return;
        }
        if (recipe.size() > *capacity) {
            registers[0] = darwin::mach::no_space;
            return;
        }
        if (!recipe.empty() &&
            !memory_.copy_in(registers[2], std::span { recipe })) {
            registers[0] = darwin::mach::memory_error;
            return;
        }
        registers[0] = memory_.write32(registers[3],
                           static_cast<std::uint32_t>(recipe.size()))
            ? darwin::mach::success : darwin::mach::memory_error;
        return;
    }
    if (trap != 70U) {
        registers[0] = darwin::mach::invalid_argument;
        return;
    }

    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto host = mach_support::resolve_name_with_right(
            *shared_state_, process_.pid, registers[0], xnu::ipc::Right::Send);
        if (!host || *host != mach_task_identity::initial_host_self_name) {
            registers[0] = darwin::mach_message::send_invalid_destination;
            return;
        }
    }
    constexpr std::uint32_t maximum_recipe_size = 5120U;
    if (registers[2] > maximum_recipe_size) {
        registers[0] = darwin::mach::invalid_argument;
        return;
    }
    const auto recipes = memory_.read_bytes(registers[1], registers[2]);
    if (!recipes) {
        registers[0] = darwin::mach::memory_error;
        return;
    }
    const auto creation = create_mach_voucher(registers[0], *recipes);
    if (creation.error != darwin::mach::success) {
        registers[0] = creation.error;
        return;
    }
    registers[0] = memory_.write32(registers[3], creation.name)
        ? darwin::mach::success : darwin::mach::memory_error;
}

} // namespace ilemu
