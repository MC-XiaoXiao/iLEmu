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
#include <optional>
#include <span>
#include <vector>

namespace ilemu {
namespace {

    constexpr std::uint32_t recipe_header_size = 16U;
    constexpr std::uint32_t key_request_offset = 32U;
    constexpr std::uint32_t count_request_offset = 36U;
    constexpr std::uint32_t data_reply_offset = 40U;

    [[nodiscard]] std::optional<std::uint32_t> read_word(
        std::span<const std::byte> bytes, std::size_t offset)
    {
        if (offset > bytes.size() || bytes.size() - offset < 4U)
            return std::nullopt;
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4U; ++index)
            value |= std::to_integer<std::uint32_t>(bytes[offset + index])
                     << (index * 8U);
        return value;
    }

    [[nodiscard]] std::span<const std::byte> recipe_for_key(
        std::span<const std::byte> recipes, std::uint32_t key)
    {
        std::size_t offset = 0;
        while (offset < recipes.size()) {
            const auto recipe_key = read_word(recipes, offset);
            const auto content_size = read_word(recipes, offset + 12U);
            if (!recipe_key || !content_size ||
                recipes.size() - offset < recipe_header_size ||
                *content_size > recipes.size() - offset - recipe_header_size) {
                return { };
            }
            const auto size = recipe_header_size + *content_size;
            if (*recipe_key == key)
                return recipes.subspan(offset, size);
            offset += size;
        }
        return { };
    }

} // namespace

bool CompatibilityKernel::dispatch_mach_voucher_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    if (request.identifier != xnu::mig::mach_voucher::id(
                                  xnu::mig::mach_voucher::Routine::
                                      extract_attr_recipe) ||
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
    const auto capacity = memory_.read32(request.address + count_request_offset);
    if (!key || !capacity) {
        registers[0] = darwin::mach_message::receive_invalid_data;
        return true;
    }

    std::vector<std::byte> recipe;
    bool known_voucher = false;
    {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto object = mach_support::resolve_name_with_right(*shared_state_,
            process_.pid, request.remote_port, xnu::ipc::Right::Send);
        if (object) {
            const auto found = shared_state_->mach_vouchers.find(*object);
            if (found != shared_state_->mach_vouchers.end()) {
                known_voucher = true;
                const auto selected = recipe_for_key(found->second, *key);
                recipe.assign(selected.begin(), selected.end());
            }
        }
    }
    const auto result = known_voucher
                            ? recipe.size() > *capacity ||
                                      recipe.size() >
                                          registers[3] - data_reply_offset
                                  ? darwin::mach::no_space
                                  : darwin::mach::success
                            : darwin::mach::invalid_argument;
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
    const auto padded_size = (recipe.size() + 3U) & ~std::size_t { 3U };
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
