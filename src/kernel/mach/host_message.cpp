// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Reply to guest Mach host-information and statistics messages.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/mach_host.defs
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/host_info.h

#include "kernel/kernel.hpp"
#include "host/calendar_notifications.hpp"
#include "kernel/darwin_abi.hpp"
#include "mach/mach_host_mig_ids.hpp"
#include "mach/mig_wire_abi.hpp"
#include "support.hpp"

#include <array>
#include <mutex>
#include <span>

namespace ilemu {

namespace {

    bool write_message_words(AddressSpace& memory, std::uint32_t address,
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

bool CompatibilityKernel::dispatch_mach_host_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    auto& registers = cpu.registers();
    const auto message_address = request.address;
    const std::optional<std::uint32_t> remote_port { request.remote_port };
    const std::optional<std::uint32_t> local_port { request.local_port };
    const std::optional<std::uint32_t> message_id { request.identifier };
    const auto write_result_reply = [&](std::uint32_t result) {
        if (registers[3] < darwin::mig_wire::simple_reply_payload_base) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return;
        }
        const std::array<std::uint32_t, 9> reply {
            darwin::mig_wire::message_bits(
                darwin::mig_wire::disposition_move_send_once),
            darwin::mig_wire::simple_reply_payload_base,
            *local_port,
            0,
            0,
            *message_id + 100U,
            0,
            1,
            result,
        };
        registers[0] = write_message_words(memory_, message_address, reply)
                           ? darwin::mach::success
                           : darwin::mach_message::receive_invalid_data;
    };
    if (*message_id == xnu::mig::mach_host::id(
                           xnu::mig::mach_host::Routine::host_request_notification)) {
        constexpr std::uint32_t request_size = 52U;
        if (registers[2] != request_size ||
            (request.bits & darwin::mig_wire::message_complex_bit) == 0U ||
            !memory_.accessible(message_address, request_size,
                MemoryPermission::Read)) {
            write_result_reply(darwin::mig::bad_arguments);
            return true;
        }
        const auto count = memory_.read32(message_address + 24U);
        const auto name = memory_.read32(message_address + 28U);
        const auto descriptor = memory_.read32(message_address + 36U);
        const auto type = memory_.read32(message_address + 48U);
        if (!count || !name || !descriptor || !type || *count != 1U ||
            *descriptor != (darwin::mig_wire::disposition_make_send_once << 16U)) {
            write_result_reply(darwin::mig::bad_arguments);
            return true;
        }
        std::uint32_t result;
        {
            const std::lock_guard lock { shared_state_->mach_mutex };
            const auto host = mach_support::resolve_name_with_right(
                *shared_state_, process_.pid, *remote_port,
                xnu::ipc::Right::Send);
            result = host && *host == mach_task_identity::initial_host_self_name
                         ? host_mig::CalendarNotifications::register_locked(
                               *shared_state_, process_.pid, *name, *type)
                         : darwin::mach::invalid_argument;
        }
        write_result_reply(result);
        return true;
    }
    if (*message_id == xnu::mig::mach_host::id(
                           xnu::mig::mach_host::Routine::host_create_mach_voucher) &&
        shared_state_->darwin_abi.mach_voucher_abi !=
            DarwinMachVoucherAbi::Unsupported) {
        constexpr std::uint32_t recipe_count_offset = 32U;
        constexpr std::uint32_t recipe_data_offset = 36U;
        constexpr std::uint32_t maximum_recipe_size = 5120U;
        constexpr std::uint32_t port_reply_size = 40U;
        if (registers[2] < recipe_data_offset ||
            registers[3] < port_reply_size) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return true;
        }
        const auto count = memory_.read32(message_address + recipe_count_offset);
        if (!count || *count > maximum_recipe_size ||
            *count > registers[2] - recipe_data_offset) {
            write_result_reply(darwin::mach::invalid_argument);
            return true;
        }
        const auto recipes = memory_.read_bytes(
            message_address + recipe_data_offset, *count);
        if (!recipes) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return true;
        }
        const auto creation = create_mach_voucher(*remote_port, *recipes);
        if (creation.error != darwin::mach::success) {
            write_result_reply(creation.error);
            return true;
        }
        const std::array<std::uint32_t, 10> reply {
            darwin::mig_wire::message_bits(
                darwin::mig_wire::disposition_move_send_once, 0, true),
            port_reply_size,
            *local_port,
            0,
            0,
            *message_id + 100U,
            1,
            creation.name,
            0,
            darwin::mig_wire::port_descriptor_metadata(
                darwin::mig_wire::disposition_move_send),
        };
        registers[0] = write_message_words(memory_, message_address, reply)
                           ? darwin::mach::success
                           : darwin::mach_message::receive_invalid_data;
        return true;
    }
    return false;
}

} // namespace ilemu
