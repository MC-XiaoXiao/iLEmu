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
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_mach_task_identity.hpp"
#include "mach/mach_host_mig_ids.hpp"
#include "mach/mig_wire_abi.hpp"
#include "support.hpp"

#include <array>
#include <span>

namespace ilemu {

using namespace mach_support;

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
    const std::optional<std::uint32_t> bits { request.bits };
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
                           xnu::mig::mach_host::Routine::host_create_mach_voucher) &&
        shared_state_->darwin_abi.mach_voucher_abi ==
            DarwinMachVoucherAbi::HostCreateWithInlineRecipes) {
        constexpr std::uint32_t recipe_count_offset = 32U;
        constexpr std::uint32_t recipe_data_offset = 36U;
        constexpr std::uint32_t maximum_recipe_size = 4096U;
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
        std::uint32_t voucher_name = 0;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto host_object = resolve_name_with_right(*shared_state_,
                process_.pid, *remote_port, xnu::ipc::Right::Send);
            if (host_object &&
                *host_object == mach_task_identity::initial_host_self_name) {
                const auto object = shared_state_->allocate_mach_object();
                if (shared_state_->mach_port_objects.create(object)) {
                    shared_state_->mach_vouchers.emplace(object, *recipes);
                    voucher_name = shared_state_->mach_namespaces
                                       .copyout(process_.pid, object,
                                           xnu::ipc::type_mask(
                                               xnu::ipc::Right::Send))
                                       .value_or(0);
                    if (voucher_name == 0) {
                        shared_state_->mach_vouchers.erase(object);
                        static_cast<void>(
                            shared_state_->mach_port_objects.erase(object));
                    }
                }
            }
        }
        if (voucher_name == 0) {
            write_result_reply(darwin::mach::resource_shortage);
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
            voucher_name,
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
