// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Resolve shared exclusive-monitor addresses across guest address
// spaces.

#include "foundation/guest_exclusive_address_resolver.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace ilemu {

void GuestExclusiveAddressResolver::bind(std::size_t processor_base,
    std::size_t processor_count, AddressSpace& memory)
{
    if (processor_count == 0 ||
        processor_count >
            std::numeric_limits<std::size_t>::max() - processor_base) {
        throw std::invalid_argument {
            "invalid exclusive monitor binding range"
        };
    }
    const auto end = processor_base + processor_count;
    const std::unique_lock lock { mutex_ };
    const auto next = std::lower_bound(bindings_.begin(), bindings_.end(),
        processor_base, [](const Binding& binding, std::size_t base) {
            return binding.begin < base;
        });
    if (next != bindings_.end() && next->begin < end) {
        throw std::logic_error { "overlapping exclusive monitor binding" };
    }
    if (next != bindings_.begin()) {
        const auto previous = std::prev(next);
        if (previous->end > processor_base) {
            throw std::logic_error { "overlapping exclusive monitor binding" };
        }
    }
    bindings_.insert(next, Binding { processor_base, end, &memory });
}

void GuestExclusiveAddressResolver::unbind(std::size_t processor_base,
    std::size_t processor_count, AddressSpace& memory) noexcept
{
    if (processor_count == 0 ||
        processor_count >
            std::numeric_limits<std::size_t>::max() - processor_base) {
        return;
    }
    const auto end = processor_base + processor_count;
    const std::unique_lock lock { mutex_ };
    const auto binding = std::lower_bound(bindings_.begin(), bindings_.end(),
        processor_base, [](const Binding& candidate, std::size_t base) {
            return candidate.begin < base;
        });
    if (binding != bindings_.end() && binding->begin == processor_base &&
        binding->end == end && binding->memory == &memory) {
        bindings_.erase(binding);
    }
}

Dynarmic::VAddr GuestExclusiveAddressResolver::resolve(
    std::size_t processor_id, Dynarmic::VAddr address) const noexcept
{
    try {
        const std::shared_lock lock { mutex_ };
        const auto next = std::upper_bound(bindings_.begin(), bindings_.end(),
            processor_id, [](std::size_t processor, const Binding& binding) {
                return processor < binding.begin;
            });
        if (next == bindings_.begin())
            return address;
        const auto binding = std::prev(next);
        if (processor_id >= binding->end || binding->memory == nullptr)
            return address;
        return binding->memory->exclusive_reservation_key(
            static_cast<std::uint32_t>(address));
    } catch (...) {
        return address;
    }
}

Dynarmic::VAddr GuestExclusiveAddressResolver::resolve_callback(
    void* context, std::size_t processor_id, Dynarmic::VAddr address) noexcept
{
    if (context == nullptr)
        return address;
    return static_cast<GuestExclusiveAddressResolver*>(context)->resolve(
        processor_id, address);
}

} // namespace ilemu
