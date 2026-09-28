// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "../transport/kernel_reply.hpp"
#include "kernel/kernel_mach_task_identity.hpp"
#include "mach/mach_host_mig_ids.hpp"
#include "kernel/mach_host_statistics_abi.hpp"
#include "mach/xnu_mig_adapter.hpp"

namespace ilemu::host_mig {

// Direct handoff and queued kernel IPC share the same host data evaluation.
// Message transport owns header rights and their cleanup on every path.
class Information {
public:
    static bool handles(std::uint32_t identifier)
    {
        using namespace xnu::mig::mach_host;
        return identifier == id(Routine::host_info) ||
               identifier == id(Routine::host_statistics) ||
               identifier == id(Routine::host_page_size);
    }
    static std::optional<std::uint32_t> dispatch_locked(
        AddressSpace& memory, KernelSharedState& state,
        std::uint32_t processor_count, std::uint64_t usable_ram,
        std::uint32_t host, KernelSharedState::MachMessage& request)
    {
        const auto result = evaluate(memory, state, processor_count, usable_ram, host, request.bytes);
        return mach_ipc::enqueue_kernel_reply_locked(state, request,
            mach_support::read_little_word(request.bytes, 20U), result.payload());
    }
    static std::optional<std::uint32_t> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state, const ProcessContext& process,
        std::uint32_t processor_count, std::uint64_t usable_ram,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address)
    {
        using namespace mach_support;
        constexpr auto send_receive = darwin::mach_message::option_send |
                                      darwin::mach_message::option_receive;
        constexpr auto options = send_receive |
            darwin::mach_message::option_send_timeout |
            darwin::mach_message::option_receive_timeout |
            darwin::mach_message::option_receive_large;
        if (bits != (19U | (21U << 8U)) ||
            (registers[1] & send_receive) != send_receive ||
            (registers[1] & ~options) != 0U || registers[4] != reply_name ||
            (registers[2] != 24U && registers[2] != 40U))
            return std::nullopt;
        const auto target = memory.read32(registers[0] + 8U);
        const auto host = target ? resolve_name_with_right(
            state, process.pid, *target, xnu::ipc::Right::Send) : std::nullopt;
        const auto reply = resolve_name_with_right(
            state, process.pid, reply_name, xnu::ipc::Right::Receive);
        if (!host || !reply ||
            *host != mach_task_identity::initial_host_self_name ||
            !state.mach_port_objects.contains(*reply) ||
            state.mach_port_set_links_by_member.contains(*reply))
            return std::nullopt;
        const auto queue = state.mach_queues.find(*reply);
        if (queue == state.mach_queues.end() || !queue->second.empty())
            return std::nullopt;
        std::array<std::byte, 40> request;
        const auto bytes = std::span { request }.first(registers[2]);
        if (!memory.copy_out(registers[0], bytes))
            return darwin::mach_message::send_invalid_data;
        const auto result = evaluate(memory, state, processor_count, usable_ram, *host, bytes);
        const auto reply_size = 32U + static_cast<std::uint32_t>(result.payload().size_bytes());
        if (registers[3] < reply_size ||
            !memory.accessible(receive_address, reply_size, MemoryPermission::Write))
            return std::nullopt;
        state.mach_port_objects.make_send_once(*reply);
        return mach_ipc::copyout_kernel_reply_locked<32>(memory, state, receive_address,
            reply_name, *reply, read_little_word(bytes, 20U), result.payload());
    }
private:
    struct Result {
        std::array<std::uint32_t, 32> words { 0U, 1U };
        std::size_t count { 3U };
        explicit Result(std::uint32_t error = 0U) { words[2] = error; }
        std::span<const std::uint32_t> payload() const { return std::span { words }.first(count); }
        static Result counted(std::span<const std::uint32_t> values)
        {
            Result result;
            result.words[3] = static_cast<std::uint32_t>(values.size());
            std::copy(values.begin(), values.end(), result.words.begin() + 4);
            result.count = 4U + values.size();
            return result;
        }
    };
    static Result evaluate(AddressSpace& memory, const KernelSharedState& state,
        std::uint32_t processor_count, std::uint64_t usable_ram,
        std::uint32_t host, std::span<const std::byte> bytes)
    {
        using namespace mach_support;
        using namespace darwin::mach::xnu;
        const auto identifier = read_little_word(bytes, 20U);
        const auto page_size = identifier == routine::host_page_size;
        if (bytes.size() != (page_size ? 24U : 40U) ||
            (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) != 0U)
            return Result { darwin::mig::bad_arguments };
        if (host != mach_task_identity::initial_host_self_name)
            return Result { darwin::mach::invalid_argument };
        if (page_size) {
            Result result;
            result.words[3] = AddressSpace::page_size;
            result.count = 4U;
            return result;
        }
        if (identifier == routine::host_info) {
            const auto flavor =
                read_little_word(bytes, message::flavor_offset);
            const auto requested_count =
                read_little_word(bytes, message::count_inout_request_offset);
            if (flavor == host_info::basic_flavor &&
                requested_count >= host_info::basic_old_word_count) {
                // The host_basic_info ABI accepts the five-word legacy prefix and
                // returns the full structure when it fits.
                const auto configured_memory_size =
                    usable_ram != 0
                        ? std::min(usable_ram,
                              state.device_ram_bytes)
                        : state.device_ram_bytes;
                const auto memory_size = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(configured_memory_size,
                        std::numeric_limits<std::uint32_t>::max()));
                const auto max_mem = state.device_ram_bytes;
                const std::array<std::uint32_t, host_info::basic_word_count> info {
                    processor_count, // max_cpus
                    processor_count, // avail_cpus
                    memory_size,
                    state.device_cpu_type,
                    state.device_cpu_subtype,
                    0, // cpu_threadtype
                    processor_count, // physical_cpu
                    processor_count, // physical_cpu_max
                    processor_count, // logical_cpu
                    processor_count, // logical_cpu_max
                    static_cast<std::uint32_t>(max_mem), // max_mem, low 32 bits
                    static_cast<std::uint32_t>(max_mem >> 32U), // high 32 bits
                };
                const auto count = requested_count >= info.size()
                                       ? info.size()
                                       : host_info::basic_old_word_count;
                return Result::counted(std::span { info }.first(count));
            }
            if (flavor == host_info::priority_flavor &&
                requested_count >= host_info::priority_word_count) {
                const std::array<std::uint32_t, host_info::priority_word_count> info {
                    80, // MINPRI_KERNEL
                    80, // MINPRI_KERNEL
                    64, // MINPRI_RESERVED
                    31, // BASEPRI_DEFAULT
                    0, // DEPRESSPRI
                    0, // IDLEPRI
                    0, // MINPRI_USER
                    79, // MAXPRI_RESERVED
                };
                return Result::counted(info);
            }
            return Result { darwin::mach::failure };
        }
        if (identifier == routine::host_statistics) {
            const auto flavor =
                read_little_word(bytes, message::flavor_offset);
            const auto requested_count =
                read_little_word(bytes, message::count_inout_request_offset);
            if (flavor == host_statistics::load_flavor &&
                requested_count >= host_statistics::load_word_count) {
                const std::array<std::uint32_t, host_statistics::load_word_count>
                    info { };
                return Result::counted(info);
            }
            if (flavor == host_statistics::vm_flavor &&
                requested_count >= host_statistics::vm_rev0_word_count) {
                const auto total_pages = std::min<std::uint64_t>(
                    state.device_ram_bytes / AddressSpace::page_size +
                        (state.device_ram_bytes % AddressSpace::page_size !=
                                0),
                    std::numeric_limits<std::uint32_t>::max());
                const auto resident_pages = std::min<std::uint64_t>(
                    memory.resident_page_count(), total_pages);
                std::array<std::uint32_t, host_statistics::vm_rev2_word_count>
                    info { };
                // vm_statistics free count includes speculative pages. The
                // emulator has no separate global VM queues, so project the
                // address space's resident pages into active memory and keep the
                // total-page invariant visible to host consumers.
                info[0] = static_cast<std::uint32_t>(total_pages - resident_pages);
                info[1] = static_cast<std::uint32_t>(resident_pages);
                const auto count = requested_count >= host_statistics::vm_rev2_word_count
                                       ? host_statistics::vm_rev2_word_count
                                       : requested_count >= host_statistics::vm_rev1_word_count
                                       ? host_statistics::vm_rev1_word_count
                                       : host_statistics::vm_rev0_word_count;
                return Result::counted(std::span { info }.first(count));
            }
            if (flavor == host_statistics::cpu_load_flavor &&
                requested_count >= host_statistics::cpu_load_word_count) {
                const std::array<std::uint32_t, host_statistics::cpu_load_word_count>
                    info { };
                return Result::counted(info);
            }
            return Result { darwin::mach::failure };
        }
        return Result { darwin::mig::bad_id };
    }
};
} // namespace ilemu::host_mig
