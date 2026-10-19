// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "information.hpp"
#include "../transport/kernel_reply.hpp"
#include "mach/thread_act_mig_ids.hpp"

namespace ilemu::thread_mig {
// XNU osfmk/mach/thread_act.defs and kern/thread.c: thread_info.
// Formatting and scheduler ownership remain shared with the existing provider.
class Information {
public:
    static constexpr auto identifier =
        xnu::mig::thread_act::id(xnu::mig::thread_act::Routine::thread_info);
    static bool handles(std::uint32_t id) { return id == identifier; }

    template <typename Query>
    static std::optional<std::uint32_t> dispatch_locked(KernelSharedState& state,
        std::uint32_t object, KernelSharedState::MachMessage& request, const Query& query)
    {
        const auto result = evaluate_locked(state, object, request.bytes, query);
        return mach_ipc::enqueue_kernel_reply_locked(
            state, request, identifier, result.payload());
    }

    template <typename Query>
    static std::optional<std::uint32_t> try_synchronous_locked(AddressSpace& memory,
        KernelSharedState& state, const ProcessContext& process,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address, const Query& query)
    {
        std::optional<std::pair<std::uint32_t, std::uint32_t>> owner;
        const auto destination = mach_ipc::validate_kernel_rpc_locked(memory, state,
            process, registers, bits, reply_name, 40U, 44U, false,
            [&](std::uint32_t object) {
                owner = mach_support::find_thread_owner(state, object);
                return owner.has_value();
            });
        if (!destination)
            return std::nullopt;
        std::array<std::byte, 40> bytes;
        if (!memory.copy_out(registers[0], bytes) ||
            mach_support::read_little_word(bytes, 16U) != 0U)
            return std::nullopt;
        const auto capacity = mach_support::read_little_word(bytes, 36U);
        const auto count = mach_support::ThreadInformation::word_count(
            mach_support::read_little_word(bytes, 32U));
        const auto size = count != 0U && capacity >= count ? 48U + 4U * count : 44U;
        if (registers[3] < size ||
            !memory.accessible(receive_address, size, MemoryPermission::Write))
            return std::nullopt;
        state.mach_port_objects.make_send_once(destination->reply_object);
        const auto result = evaluate_locked(state, destination->task_object, bytes, query, owner);
        return mach_ipc::copyout_kernel_reply_locked<14>(memory, state, receive_address,
            reply_name, destination->reply_object, identifier, result.payload());
    }

private:
    struct Result {
        std::array<std::uint32_t, 14> words { 0U, 1U };
        std::size_t count { 3U };
        explicit Result(std::uint32_t error = 0U) { words[2] = error; }
        std::span<const std::uint32_t> payload() const
        { return std::span { words }.first(count); }
    };

    template <typename Query>
    static Result evaluate_locked(KernelSharedState& state, std::uint32_t object,
        std::span<const std::byte> bytes, const Query& query,
        std::optional<std::pair<std::uint32_t, std::uint32_t>> owner = std::nullopt)
    {
        using namespace mach_support;
        if (bytes.size() != 40U ||
            (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) != 0U)
            return Result { darwin::mig::bad_arguments };
        if (!owner)
            owner = find_thread_owner(state, object);
        const auto flavor = read_little_word(bytes, 32U);
        const auto capacity = read_little_word(bytes, 36U);
        const auto count = ThreadInformation::word_count(flavor);
        if (!owner || count == 0U || capacity < count || !query)
            return Result { darwin::mach::invalid_argument };
        const auto statistics = query(owner->first, owner->second);
        if (!statistics)
            return Result { darwin::mach::invalid_argument };
        const ThreadInformation information { *statistics };
        const auto error = information.result(flavor);
        if (error != darwin::mach::success)
            return Result { error };
        Result result;
        result.words[3] = static_cast<std::uint32_t>(count);
        result.count = 4U + count;
        information.write(flavor, std::span { result.words }.subspan(4U, count));
        return result;
    }
};
} // namespace ilemu::thread_mig
