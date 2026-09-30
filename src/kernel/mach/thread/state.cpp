// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Read and update guest ARM thread register-state flavors.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/thread_act.defs

#include "kernel/kernel.hpp"

#include "kernel/mach_arm_thread_abi.hpp"
#include "kernel/mach_thread_info_abi.hpp"
#include "mach/mig_wire_abi.hpp"
#include "mach/thread_act_mig_ids.hpp"
#include "mach/xnu_scheduler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../support.hpp"
#include "state_server.hpp"
#include "information.hpp"

namespace ilemu {
namespace {

    using namespace mach_support;

    constexpr std::uint32_t mach_message_success = 0;
    constexpr std::uint32_t mach_receive_invalid_data = 0x10004008U;
    constexpr std::uint32_t kernel_invalid_argument = 4;
    constexpr std::uint32_t simple_reply_size = 36;
    constexpr std::uint32_t state_reply_prefix_size = 40;

    bool write_words(AddressSpace& memory, std::uint32_t address,
        std::span<const std::uint32_t> words)
    {
        for (std::size_t index = 0; index < words.size(); ++index) {
            if (!memory.write32(address + static_cast<std::uint32_t>(
                                              index * sizeof(std::uint32_t)),
                    words[index])) {
                return false;
            }
        }
        return true;
    }

} // namespace

bool CompatibilityKernel::dispatch_mach_thread_state_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    const auto gets_info =
        request.identifier ==
        mig_message_id(xnu::mig::thread_act::Routine::thread_info);
    const auto gets_state =
        request.identifier ==
        mig_message_id(xnu::mig::thread_act::Routine::thread_get_state) ||
        request.identifier ==
        mig_message_id(xnu::mig::thread_act::Routine::act_get_state);
    const auto sets_state =
        request.identifier ==
            mig_message_id(
                xnu::mig::thread_act::Routine::thread_set_state) ||
        request.identifier ==
            mig_message_id(xnu::mig::thread_act::Routine::act_set_state);
    if (!gets_info && !gets_state && !sets_state) {
        return false;
    }

    auto& registers = cpu.registers();
    if (gets_info) {
        const auto& arguments = xnu::mig::thread_act::thread_info_arguments;
        const auto flavor =
            memory_.read32(request.address + arguments[1].request_offset);
        const auto capacity =
            memory_.read32(request.address + arguments[2].request_count_offset);
        std::optional<std::pair<std::uint32_t, std::uint32_t>> target_owner;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto object = resolve_name_with_right(*shared_state_,
                process_.pid, request.remote_port, xnu::ipc::Right::Send);
            if (object) {
                target_owner = find_thread_owner(*shared_state_, *object);
            }
        }

        const auto requested_word_count =
            flavor ? ThreadInformation::word_count(*flavor) : 0U;
        std::optional<XnuThreadStatistics> statistics;
        if (flavor && capacity && target_owner && requested_word_count != 0U &&
            *capacity >= requested_word_count && thread_statistics_query_)
            statistics = thread_statistics_query_(target_owner->first, target_owner->second);
        const auto kernel_result = statistics
                                       ? ThreadInformation { *statistics }.result(*flavor)
                                       : kernel_invalid_argument;
        if (kernel_result != 0) {
            const std::array<std::uint32_t,
                simple_reply_size / sizeof(std::uint32_t)>
                reply { darwin::mig_wire::message_bits(
                            darwin::mig_wire::disposition_move_send_once),
                    simple_reply_size, request.local_port, 0, 0,
                    request.identifier + 100, 0, 1, kernel_result };
            registers[0] = write_words(memory_, request.address, reply)
                               ? mach_message_success
                               : mach_receive_invalid_data;
            output_.write(
                "[mach] thread_info caller=" + std::to_string(process_.pid) +
                " flavor=" + std::to_string(flavor.value_or(0)) + " capacity=" +
                std::to_string(capacity.value_or(0)) + " result=" + std::to_string(kernel_result) + "\n");
            return true;
        }

        const auto reply_size = static_cast<std::uint32_t>(
            state_reply_prefix_size +
            requested_word_count * sizeof(std::uint32_t));
        if (registers[3] < reply_size) {
            registers[0] = mach_receive_invalid_data;
            return true;
        }
        std::vector<std::uint32_t> reply {
            darwin::mig_wire::message_bits(
                darwin::mig_wire::disposition_move_send_once),
            reply_size,
            request.local_port,
            0,
            0,
            request.identifier + 100,
            0,
            1,
            mach_message_success,
            static_cast<std::uint32_t>(requested_word_count),
        };
        reply.resize(state_reply_prefix_size / sizeof(std::uint32_t) +
                     requested_word_count);
        ThreadInformation { *statistics }.write(*flavor,
            std::span { reply }.subspan(state_reply_prefix_size / sizeof(std::uint32_t)));
        registers[0] = write_words(memory_, request.address, reply)
                           ? mach_message_success
                           : mach_receive_invalid_data;
        output_.write(
            "[mach] thread_info caller=" + std::to_string(process_.pid) +
            " flavor=" + std::to_string(*flavor) +
            " count=" + std::to_string(requested_word_count) + " result=0\n");
        return true;
    }

    if (!pending_mach_receives_.empty())
        return false;
    std::lock_guard mach_lock { shared_state_->mach_mutex };
    const auto result = thread_mig::State::try_synchronous_locked(memory_, cpu,
        *shared_state_, process_, registers, request.bits, request.local_port,
        request.address, request.identifier, thread_state_query_, thread_state_update_handler_);
    if (!result)
        return false;
    registers[0] = *result;
    return true;
}

} // namespace ilemu
