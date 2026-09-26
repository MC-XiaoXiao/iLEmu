// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../mach/transport/kernel_reply.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "kernel/mach_clock_abi.hpp"
#include "mach/clock_mig_ids.hpp"
#include <array>

namespace ilemu::kernel_clock {
class QueryServer {
public:
    static bool handles(std::uint32_t identifier)
    {
        using namespace xnu::mig::clock;
        return identifier == id(Routine::clock_get_time) ||
               identifier == id(Routine::clock_get_attributes);
    }

    // Called after generic Mach copyin has resolved the destination capability.
    static std::optional<std::uint32_t> identify_locked(
        const KernelSharedState& state, const ProcessContext& process,
        std::uint32_t object, std::uint32_t identifier)
    {
        using namespace xnu::mig::clock;
        if (!handles(identifier))
            return std::nullopt;
        if (state.mach_namespaces.resolve(process.pid, process.clock_port) ==
            object)
            return darwin::mach::clock::system_clock_id;
        if (state.mach_namespaces.resolve(
                process.pid, process.calendar_clock_port) == object)
            return darwin::mach::clock::calendar_clock_id;
        return std::nullopt;
    }

    static std::array<std::uint32_t, 5> evaluate(KernelSharedState& state,
        std::uint32_t clock, std::span<const std::byte> bytes)
    {
        using namespace xnu::mig::clock;
        using namespace mach_support;
        const auto identifier = read_little_word(bytes, 20U);
        const auto time_query = identifier == id(Routine::clock_get_time);
        const auto expected_size = time_query ? 24U : 40U;
        std::array<std::uint32_t, 5> payload { 0U, 1U, 0U, 0U, 0U };
        if ((read_little_word(bytes, 0U) &
                darwin::mig_wire::message_complex_bit) != 0 ||
            bytes.size() != expected_size) {
            payload[2] = darwin::mig::bad_arguments;
        } else if (time_query) {
            const auto now = clock == darwin::mach::clock::calendar_clock_id
                                 ? state.clock.wall_time()
                                 : state.clock.now();
            payload[3] = static_cast<std::uint32_t>(
                now / darwin::mach::clock::nanoseconds_per_second);
            payload[4] = static_cast<std::uint32_t>(
                now % darwin::mach::clock::nanoseconds_per_second);
        } else {
            const auto flavor = read_little_word(bytes, 32U);
            const auto capacity = read_little_word(bytes, 36U);
            // MIG clamps the CountInOut array to its single-word maximum.
            if (capacity == 0U) {
                payload[2] = darwin::mach::failure;
            } else if (
                flavor != darwin::mach::clock::get_time_resolution_flavor &&
                flavor !=
                    darwin::mach::clock::alarm_current_resolution_flavor &&
                flavor !=
                    darwin::mach::clock::alarm_minimum_resolution_flavor &&
                flavor !=
                    darwin::mach::clock::alarm_maximum_resolution_flavor) {
                payload[2] = darwin::mach::invalid_value;
            } else {
                payload[3] = darwin::mach::clock::attribute_word_count;
                payload[4] =
                    darwin::mach::clock::virtual_resolution_nanoseconds;
            }
        }
        return payload;
    }

    static std::optional<std::uint32_t> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state,
        const ProcessContext& process, std::uint32_t clock,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address)
    {
        using namespace mach_support;
        // COPY_SEND has no net right mutation; MAKE_SEND_ONCE is consumed by
        // the immediate receive. Other dispositions use full generic copyin.
        constexpr auto send_receive = darwin::mach_message::option_send |
                                      darwin::mach_message::option_receive;
        constexpr auto rpc_options =
            send_receive | darwin::mach_message::option_receive_timeout |
            darwin::mach_message::option_receive_large;
        if (bits != (darwin::mig_wire::disposition_copy_send |
                        (darwin::mig_wire::disposition_make_send_once << 8U)) ||
            (registers[1] & send_receive) != send_receive ||
            (registers[1] & ~rpc_options) != 0U ||
            (registers[2] != 24U && registers[2] != 40U) ||
            registers[3] < 52U || registers[4] != reply_name)
            return std::nullopt;
        const auto reply = resolve_name_with_right(
            state, process.pid, reply_name, xnu::ipc::Right::Receive);
        if (!reply || !state.mach_port_objects.contains(*reply) ||
            state.mach_port_set_links_by_member.contains(*reply))
            return std::nullopt;
        const auto queue = state.mach_queues.find(*reply);
        if (queue == state.mach_queues.end() || !queue->second.empty())
            return std::nullopt;
        std::array<std::byte, 40> request;
        const auto bytes = std::span { request }.first(registers[2]);
        if (!memory.copy_out(registers[0], bytes))
            return darwin::mach_message::send_invalid_data;
        const auto payload = evaluate(state, clock, bytes);
        return mach_ipc::copyout_simple_kernel_reply_locked(memory, state,
            receive_address, reply_name, *reply, read_little_word(bytes, 20U),
            std::span { payload }.first(payload[2] == 0U ? 5U : 3U));
    }

    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t clock,
        KernelSharedState::MachMessage& request)
    {
        const auto payload = evaluate(state, clock, request.bytes);
        return mach_ipc::enqueue_kernel_reply_locked(state, request,
            mach_support::read_little_word(request.bytes, 20U),
            std::span<const std::uint32_t> { payload }.first(
                payload[2] == 0 ? 5U : 3U));
    }
};
} // namespace ilemu::kernel_clock
