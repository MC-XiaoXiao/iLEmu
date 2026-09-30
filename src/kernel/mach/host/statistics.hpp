// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "information_result.hpp"
#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "kernel/mach_host_statistics_abi.hpp"

namespace ilemu::host_mig {

// One evaluator for both transport paths and both statistics word layouts.
class Statistics {
public:
    static InformationResult evaluate(AddressSpace& memory,
        const KernelSharedState& state, std::uint32_t flavor,
        std::uint32_t requested_count, bool wide)
    {
        using namespace darwin::mach::xnu;
        const host_statistics::Contract contract { state.darwin_abi.abi_epoch };
        if (wide && flavor == host_statistics::vm64_flavor) {
            const auto count = contract.wide_vm_count(requested_count);
            if (count == 0U)
                return InformationResult { darwin::mach::failure };
            std::array<std::uint32_t, host_statistics::vm64_rev1_word_count> info { };
            resident_counts(memory, state, info);
            return InformationResult::counted(std::span { info }.first(count));
        }
        if (wide && flavor == host_statistics::extmod64_flavor &&
            contract.supports_external_modifications()) {
            if (requested_count < host_statistics::extmod64_word_count)
                return InformationResult { darwin::mach::failure };
            // Deferred: remote task/thread modifications are not yet accounted.
            // Do not report successful zero counters for events we do implement.
            return InformationResult { darwin::mach::not_supported };
        }
        if (flavor == host_statistics::load_flavor &&
            requested_count >= host_statistics::load_word_count) {
            const std::array<std::uint32_t, host_statistics::load_word_count>
                info { };
            return InformationResult::counted(info);
        }
        if (flavor == host_statistics::vm_flavor &&
            requested_count >= host_statistics::vm_rev0_word_count) {
            std::array<std::uint32_t, host_statistics::vm_rev2_word_count> info { };
            resident_counts(memory, state, info);
            const auto count = requested_count >= host_statistics::vm_rev2_word_count
                                   ? host_statistics::vm_rev2_word_count
                                   : requested_count >= host_statistics::vm_rev1_word_count
                                   ? host_statistics::vm_rev1_word_count
                                   : host_statistics::vm_rev0_word_count;
            return InformationResult::counted(std::span { info }.first(count));
        }
        if (flavor == host_statistics::cpu_load_flavor &&
            requested_count >= host_statistics::cpu_load_word_count) {
            const std::array<std::uint32_t, host_statistics::cpu_load_word_count>
                info { };
            return InformationResult::counted(info);
        }
        switch (flavor) {
        case host_statistics::load_flavor:
        case host_statistics::vm_flavor:
        case host_statistics::cpu_load_flavor:
            return InformationResult { darwin::mach::failure };
        default:
            return InformationResult { darwin::mach::invalid_argument };
        }
    }
private:
    static void resident_counts(AddressSpace& memory, const KernelSharedState& state,
        std::span<std::uint32_t> info)
    {
        const auto total_pages = std::min<std::uint64_t>(
            state.device_ram_bytes / AddressSpace::page_size +
                (state.device_ram_bytes % AddressSpace::page_size != 0),
            std::numeric_limits<std::uint32_t>::max());
        const auto resident_pages = std::min<std::uint64_t>(
            memory.resident_page_count(), total_pages);
        // Preserve the existing resident-page projection for both ABIs. There
        // are no global VM queues/event counters yet; this is not host-wide
        // deduplicated accounting. Do not scan other address spaces per RPC.
        info[0] = static_cast<std::uint32_t>(total_pages - resident_pages);
        info[1] = static_cast<std::uint32_t>(resident_pages);
    }
};
} // namespace ilemu::host_mig
