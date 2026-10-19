// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Shared voucher creation for the host MIG request and direct Mach trap.

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_mach_task_identity.hpp"

#include <mutex>
#include <utility>

#include "support.hpp"

namespace ilemu {

CompatibilityKernel::MachVoucherCreation CompatibilityKernel::create_mach_voucher(
    std::uint32_t host_name, std::span<const std::byte> recipes)
{
    std::lock_guard mach_lock { shared_state_->mach_mutex };
    const auto host_object = mach_support::resolve_name_with_right(
        *shared_state_, process_.pid, host_name, xnu::ipc::Right::Send);
    if (!host_object ||
        *host_object != mach_task_identity::initial_host_self_name)
        return { darwin::mach::invalid_argument, 0 };

    MachVoucherState voucher;
    const auto result = voucher.create(recipes,
        [&](std::uint32_t name) -> const MachVoucherState* {
            const auto object = mach_support::resolve_name_with_right(
                *shared_state_, process_.pid, name, xnu::ipc::Right::Send);
            if (!object)
                return nullptr;
            const auto found = shared_state_->mach_vouchers.find(*object);
            return found == shared_state_->mach_vouchers.end()
                ? nullptr : &found->second;
        }, shared_state_->mach_vouchers, shared_state_->next_activity_id,
        process_.pid);
    if (result != darwin::mach::success)
        return { result, 0 };

    // XNU's all-default voucher is MACH_VOUCHER_NULL.
    if (voucher.empty())
        return { darwin::mach::success, 0 };

    std::uint32_t object = 0;
    for (const auto& [candidate, value] : shared_state_->mach_vouchers) {
        if (value == voucher) {
            object = candidate;
            break;
        }
    }
    const bool created = object == 0;
    if (created) {
        object = shared_state_->allocate_mach_object();
        if (shared_state_->mach_port_objects.create(object))
            shared_state_->mach_vouchers.emplace(object, std::move(voucher));
        else
            object = 0;
    }
    if (!object)
        return { darwin::mach::resource_shortage, 0 };

    const auto name = shared_state_->mach_namespaces.copyout(
        process_.pid, object, xnu::ipc::type_mask(xnu::ipc::Right::Send))
                          .value_or(0);
    if (!name && created) {
        shared_state_->mach_vouchers.erase(object);
        static_cast<void>(shared_state_->mach_port_objects.erase(object));
    }
    return { name ? darwin::mach::success : darwin::mach::resource_shortage,
        name };
}

} // namespace ilemu
