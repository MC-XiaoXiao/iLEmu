// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <foundation/address_space.hpp>
#include <kernel/kernel_shared_state.hpp>
#include <kernel/task_memory_statistics.hpp>
#include <mach/task_mig_ids.hpp>
#include <array>

namespace ilemu::task_mig {
class Information {
public:
    static constexpr auto identifier =
        xnu::mig::task::id(xnu::mig::task::Routine::task_info);
    static bool handles(std::uint32_t id) { return id == identifier; }
    static std::optional<std::uint32_t> dispatch_locked(AddressSpace& memory,
        KernelSharedState& state, std::uint32_t caller, std::uint32_t object,
        KernelSharedState::MachMessage& request, const TaskMemoryStatisticsQuery& query);
    static std::optional<std::uint32_t> try_synchronous_locked(AddressSpace& memory,
        KernelSharedState& state, const ProcessContext& process,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address,
        const TaskMemoryStatisticsQuery& query);
private:
    struct Result {
        std::array<std::uint32_t, 12> words { 0U, 1U };
        std::size_t count { 3U };
        explicit Result(std::uint32_t error = 0U) { words[2] = error; }
        std::span<const std::uint32_t> payload() const
        { return std::span { words }.first(count); }
    };
    static std::size_t word_count(std::uint32_t flavor, std::uint32_t capacity);
    static Result evaluate_locked(AddressSpace& memory, KernelSharedState& state,
        std::uint32_t caller, std::uint32_t object, std::span<const std::byte> bytes,
        const TaskMemoryStatisticsQuery& query);
};
} // namespace ilemu::task_mig
