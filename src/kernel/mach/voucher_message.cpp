// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Return Mach voucher attribute recipes through the guest MIG wire ABI.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.1.97/osfmk/ipc/ipc_voucher.c

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"
#include "mach/mach_voucher_mig_ids.hpp"
#include "mach/mig_wire_abi.hpp"

#include "support.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ilemu {
namespace {

    constexpr std::uint32_t key_request_offset = 32U;
    constexpr std::uint32_t count_request_offset = 36U;
    constexpr std::uint32_t data_reply_offset = 40U;

} // namespace

bool CompatibilityKernel::dispatch_mach_voucher_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    using namespace xnu::mig::mach_voucher;
    const auto content_only = request.identifier == id(Routine::extract_attr_content);
    const auto attribute_command = request.identifier == id(Routine::attr_command);
    if ((!content_only && !attribute_command &&
            request.identifier != id(Routine::extract_attr_recipe)) ||
        shared_state_->darwin_abi.mach_voucher_abi !=
            DarwinMachVoucherAbi::HostCreateWithInlineRecipes) {
        return false;
    }
    auto& registers = cpu.registers();
    if (registers[2] < data_reply_offset ||
        registers[3] < data_reply_offset) {
        registers[0] = darwin::mach_message::receive_invalid_data;
        return true;
    }
    const auto key = memory_.read32(request.address + key_request_offset);
    auto capacity = memory_.read32(request.address + count_request_offset);
    std::uint32_t command = 0;
    std::vector<std::byte> input;
    if (attribute_command) {
        const auto count = memory_.read32(request.address + 40U);
        if (registers[2] < 48U || !count || *count > 4096U ||
            ((*count + 3U) & ~3U) > registers[2] - 48U) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return true;
        }
        command = capacity.value_or(0);
        const auto bytes = memory_.read_bytes(request.address + 44U, *count);
        if (!bytes || !capacity) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return true;
        }
        input = *bytes;
        capacity = memory_.read32(request.address + 44U + ((*count + 3U) & ~3U));
    }
    if (!key || !capacity) {
        registers[0] = darwin::mach_message::receive_invalid_data;
        return true;
    }

    std::vector<std::byte> recipe;
    auto result = darwin::mach::invalid_argument;
    {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto object = mach_support::resolve_name_with_right(*shared_state_,
            process_.pid, request.remote_port, xnu::ipc::Right::Send);
        if (object) {
            const auto found = shared_state_->mach_vouchers.find(*object);
            if (found != shared_state_->mach_vouchers.end()) {
                if (attribute_command) {
                    result = found->second.command(*key, command, input,
                        *capacity, process_.pid, shared_state_->next_subactivity_id, recipe);
                } else {
                    recipe = found->second.extract(*key, !content_only);
                    result = darwin::mach::success;
                }
            }
        }
    }
    if (result == darwin::mach::success &&
        (recipe.size() > *capacity ||
            ((recipe.size() + 3U) & ~std::size_t { 3U }) > registers[3] - data_reply_offset))
        result = darwin::mach::no_space;
    const auto size = result == darwin::mach::success
                          ? static_cast<std::uint32_t>(
                                data_reply_offset + ((recipe.size() + 3U) & ~3U))
                          : darwin::mig_wire::simple_reply_payload_base;
    const std::array<std::uint32_t, 10> reply {
        darwin::mig_wire::message_bits(
            darwin::mig_wire::disposition_move_send_once),
        size,
        request.local_port,
        0,
        0,
        request.identifier + 100U,
        0,
        1,
        result,
        static_cast<std::uint32_t>(
            result == darwin::mach::success ? recipe.size() : 0U),
    };
    const auto word_count = result == darwin::mach::success ? reply.size() : 9U;
    for (std::size_t index = 0; index < word_count; ++index) {
        if (!memory_.write32(request.address +
                static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
                reply[index])) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return true;
        }
    }
    const auto padded_size = result == darwin::mach::success
        ? (recipe.size() + 3U) & ~std::size_t { 3U } : 0U;
    for (std::size_t index = 0; index < padded_size; ++index) {
        if (!memory_.write8(request.address + data_reply_offset +
                static_cast<std::uint32_t>(index),
                index < recipe.size()
                    ? std::to_integer<std::uint8_t>(recipe[index])
                    : 0U)) {
            registers[0] = darwin::mach_message::receive_invalid_data;
            return true;
        }
    }
    registers[0] = darwin::mach::success;
    return true;
}

} // namespace ilemu
