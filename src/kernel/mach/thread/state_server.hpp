// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "../transport/kernel_reply.hpp"
#include "kernel/mach_arm_thread_state.hpp"
#include "mach/thread_act_mig_ids.hpp"

namespace ilemu::thread_mig {

// XNU thread_act.defs: counted ARM32 thread_state_t, at most 144 words.
// Both transports validate the complete MIG envelope before touching a thread.
class State {
    using Owner = std::pair<std::uint32_t, std::uint32_t>;
    using Routine = xnu::mig::thread_act::Routine;
    static constexpr auto get_id = xnu::mig::thread_act::id(Routine::thread_get_state);
    static constexpr auto set_id = xnu::mig::thread_act::id(Routine::thread_set_state);
    static constexpr auto act_id = xnu::mig::thread_act::id(Routine::act_set_state);
    static constexpr auto maximum_state_words = darwin::arm_thread::maximum_state_word_count;
    static constexpr std::uint32_t maximum_request_size = 40U + 4U * maximum_state_words;
    static constexpr auto state_words = darwin::arm_thread::general_state_word_count;
    static constexpr auto payload_words = 4U + maximum_state_words;

    static std::uint32_t reply_count(std::uint32_t flavor, std::uint32_t count)
    {
        using namespace darwin::arm_thread;
        if (flavor == general_state_flavor)
            return count >= state_words ? state_words : 0U;
        if (flavor != floating_state_flavor || count < floating_prefix_word_count)
            return 0U;
        return count < floating_state_word_count ? floating_prefix_word_count
            : std::min<std::uint32_t>(count, maximum_state_words);
    }

    static std::size_t transfer_count(std::uint32_t flavor, std::uint32_t count)
    {
        using namespace darwin::arm_thread;
        return flavor == general_state_flavor ? state_words
            : count < floating_state_word_count ? floating_prefix_word_count - 1U
                                                : floating_state_word_count;
    }

public:
    static bool handles(std::uint32_t id)
    { return id == get_id || id == set_id || id == act_id; }

    template <typename Processor, typename Query, typename Update>
    static std::optional<std::uint32_t> dispatch_locked(Processor& cpu,
        KernelSharedState& state, const ProcessContext& process,
        std::uint32_t object, KernelSharedState::MachMessage& request,
        const Query& query, const Update& update)
    {
        const auto id = mach_support::read_little_word(request.bytes, 20U);
        const auto result = evaluate_locked(cpu, state, process, object,
            id, request.bytes, query, update);
        return mach_ipc::enqueue_kernel_reply_locked(state, request, id, result.payload());
    }

    template <typename Processor, typename Query, typename Update>
    static std::optional<std::uint32_t> try_synchronous_locked(AddressSpace& memory,
        Processor& cpu, KernelSharedState& state, const ProcessContext& process,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address, std::uint32_t id,
        const Query& query, const Update& update)
    {
        const auto size = registers[2];
        if (size < 40U || size > maximum_request_size)
            return std::nullopt;
        std::optional<Owner> owner;
        const auto destination = mach_ipc::validate_kernel_rpc_locked(memory, state,
            process, registers, bits, reply_name, size, 44U, false,
            [&](std::uint32_t object) {
                owner = mach_support::find_thread_owner(state, object);
                return owner.has_value();
            });
        if (!destination)
            return std::nullopt;
        std::array<std::byte, maximum_request_size> storage;
        const auto bytes = std::span { storage }.first(size);
        if (!memory.copy_out(registers[0], bytes) ||
            mach_support::read_little_word(bytes, 16U) != 0U)
            return std::nullopt;
        const auto flavor = mach_support::read_little_word(bytes, 32U);
        const auto count = mach_support::read_little_word(bytes, 36U);
        const auto output_count = id == get_id && size == 40U
            ? reply_count(flavor, count) : 0U;
        const auto reply_size = output_count ? 48U + 4U * output_count : 44U;
        // Preflight before any update: falling back must not apply it twice.
        if (registers[3] < reply_size ||
            !memory.accessible(receive_address, reply_size, MemoryPermission::Write))
            return std::nullopt;
        state.mach_port_objects.make_send_once(destination->reply_object);
        const auto result = evaluate_locked(cpu, state, process,
            destination->task_object, id, bytes, query, update, owner);
        return mach_ipc::copyout_kernel_reply_locked<payload_words>(memory, state,
            receive_address, reply_name, destination->reply_object, id, result.payload());
    }

private:
    struct Result {
        std::array<std::uint32_t, payload_words> words { 0U, 1U };
        std::size_t count { 3U };
        explicit Result(std::uint32_t error = 0U) { words[2] = error; }
        std::span<const std::uint32_t> payload() const
        { return std::span { words }.first(count); }
    };

    template <typename Processor, typename Query, typename Update>
    static Result evaluate_locked(Processor& cpu, KernelSharedState& state,
        const ProcessContext& process, std::uint32_t object, std::uint32_t id,
        std::span<const std::byte> bytes, const Query& query, const Update& update,
        std::optional<Owner> owner = std::nullopt)
    {
        using namespace mach_support;
        if (bytes.size() < 40U ||
            (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) != 0U)
            return Result { darwin::mig::bad_arguments };
        const auto count = read_little_word(bytes, 36U);
        if (id == get_id ? bytes.size() != 40U
                         : count > maximum_state_words || bytes.size() != 40U + 4U * count)
            return Result { darwin::mig::bad_arguments };
        if (!owner)
            owner = find_thread_owner(state, object);
        const auto flavor = read_little_word(bytes, 32U);
        const auto output_count = reply_count(flavor, count);
        if (!owner || output_count == 0U)
            return Result { darwin::mach::invalid_argument };
        const auto current = owner->first == process.pid && owner->second == cpu.processor_id();
        const auto register_count = transfer_count(flavor, count);
        if (id != get_id) {
            // act_set_state[_from_user] explicitly excludes current_thread().
            if (id == act_id && current)
                return Result { darwin::mach::invalid_argument };
            std::array<std::uint32_t, darwin::arm_thread::floating_state_word_count> requested;
            for (std::size_t i = 0; i < register_count; ++i)
                requested[i] = read_little_word(bytes, 40U + 4U * i);
            return Result { update && update(owner->first, owner->second, flavor,
                    std::span { requested }.first(register_count))
                    ? darwin::mach::success : darwin::mach::invalid_argument };
        }
        Result result;
        result.words[3] = output_count;
        result.count = 4U + output_count;
        auto output = std::span { result.words }.subspan(4U, output_count);
        const auto registers = output.first(register_count);
        const auto success = query
            ? query(owner->first, owner->second, flavor, registers)
            : current && darwin::arm_thread::read_state(cpu, flavor, registers);
        if (!success)
            return Result { darwin::mach::invalid_argument };
        return result;
    }
};
} // namespace ilemu::thread_mig
