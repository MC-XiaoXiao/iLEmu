// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/userland_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"

#include <algorithm>
#include <bit>
#include <span>

namespace ilemu {
namespace {

    std::optional<std::uint32_t> arm_immediate(std::uint32_t value)
    {
        for (unsigned rotation = 0; rotation < 16U; ++rotation) {
            const auto byte = std::rotl(value, static_cast<int>(2U * rotation));
            if (byte <= 0xffU)
                return (rotation << 8U) | byte;
        }
        return std::nullopt;
    }

    void append_word(std::vector<std::byte>& code, std::uint32_t word)
    {
        for (unsigned shift = 0; shift < 32U; shift += 8U)
            code.push_back(static_cast<std::byte>(word >> shift));
    }

    bool relocatable_frame(const std::vector<std::byte>& code)
    {
        const auto word = [&](std::size_t offset) {
            std::uint32_t result = 0;
            for (unsigned i = 0; i < 4U; ++i)
                result |= std::to_integer<std::uint32_t>(code[offset + i])
                          << (8U * i);
            return result;
        };
        const auto push = word(0U);
        const auto frame = word(4U);
        const auto scratch = word(8U);
        // Unconditional push including LR, excluding SP/PC; then mov
        // r4..r11,sp. Neither instruction refers to its address or changes
        // condition flags. The third instruction assigns IP from an argument,
        // so the entry jump may borrow IP without changing the original path.
        const auto target = (frame >> 12U) & 0xfU;
        return (push & 0xffff0000U) == 0xe92d0000U && (push & 0xa000U) == 0U &&
               (push & 0x4000U) != 0U && (frame & 0xffff0fffU) == 0xe1a0000dU &&
               target >= 4U && target <= 11U &&
               (scratch & 0xfffffff0U) == 0xe1a0c000U && (scratch & 0xfU) <= 3U;
    }

} // namespace

bool UserlandHleCall::guard_unsigned_argument(
    std::size_t index, std::uint32_t minimum, std::uint32_t maximum)
{
    const AddressSpace::ExclusiveAccess access { memory_ };
    const auto entry = cpu_.registers()[15] - 4U;
    const auto installed = registry_.installed_calls_.find(entry);
    if (installed == registry_.installed_calls_.end() ||
        installed->second.thumb || installed->second.prefix_arguments != 0U ||
        index >= 4U || minimum > maximum)
        return false;
    if (installed->second.original_entry != 0U)
        return true;
    const auto* registration =
        registry_.find_registration(installed->second.id);
    const auto low = arm_immediate(minimum);
    const auto high = arm_immediate(maximum);
    if (!registration ||
        registration->patch !=
            UserlandHleRegistry::EntryPatch::InstructionFetch ||
        !low || !high || installed->second.original.size() != 4U)
        return false;
    if (registry_.installed_calls_.contains(entry + 4U) ||
        registry_.installed_calls_.contains(entry + 8U))
        return false;
    auto original = memory_.read_bytes(entry, 12U);
    if (!original || !relocatable_frame(*original) ||
        !std::equal(installed->second.original.begin(),
            installed->second.original.end(), original->begin()))
        return false;

    const auto address = registry_.persistent_trampoline_cursor_;
    constexpr std::uint32_t code_size = 40U;
    if (address > 0x61000000U - code_size)
        return false;
    std::vector<std::byte> code;
    code.reserve(code_size);
    append_word(
        code, 0xe3500000U | (static_cast<std::uint32_t>(index) << 16U) | *low);
    append_word(code, 0x3a000002U); // blo original prologue
    append_word(
        code, 0xe3500000U | (static_cast<std::uint32_t>(index) << 16U) | *high);
    append_word(code, 0x8a000000U); // bhi original prologue
    append_word(
        code, 0xef000000U | userland_hle_svc_namespace | installed->second.id);
    code.insert(code.end(), original->begin(), original->end());
    append_word(code, 0xe51ff004U); // ldr pc,[pc,#-4]
    append_word(code, entry + 12U);
    const auto first_page = address & ~(AddressSpace::page_size - 1U);
    const auto last_page =
        (address + code_size - 1U) & ~(AddressSpace::page_size - 1U);
    for (auto page = first_page;; page += AddressSpace::page_size) {
        if (!memory_.mapped(page, AddressSpace::page_size) &&
            !memory_.map(page, AddressSpace::page_size,
                MemoryPermission::Read | MemoryPermission::Write |
                    MemoryPermission::Execute))
            return false;
        if (page == last_page)
            break;
    }
    if (!memory_.copy_in(address, code))
        return false;
    auto guarded = installed->second;
    guarded.original = *original;
    guarded.original_entry = entry;
    const auto [alias, inserted] =
        registry_.installed_calls_.emplace(address + 16U, std::move(guarded));
    if (!inserted)
        return false;
    std::vector<std::byte> patch;
    // Instruction overlays are deliberately invisible to data loads. Build
    // the absolute target in volatile IP, rather than loading a literal from
    // the overlaid entry. The validated original prologue reassigns IP.
    append_word(patch, 0xe300c000U | ((address & 0xf000U) << 4U) |
                           (address & 0xfffU)); // movw ip,lo16
    append_word(patch, 0xe340c000U | ((address & 0xf0000000U) >> 12U) |
                           ((address >> 16U) & 0xfffU)); // movt ip,hi16
    append_word(patch, 0xe12fff1cU); // bx ip
    const AddressSpace::CopyInOperation operation { entry, patch };
    if (!memory_.overlay_instructions({ &operation, 1U })) {
        registry_.installed_calls_.erase(alias);
        return false;
    }
    // Preserve the source symbol's storage: the active call has a string_view
    // into it. Map insertion leaves the dispatcher's iterator valid as well.
    installed->second.original = std::move(*original);
    installed->second.original_entry = entry;
    registry_.persistent_trampolines_.erase(entry);
    registry_.persistent_trampoline_cursor_ += code_size;
    cpu_.invalidate_cache_range(address, code_size);
    cpu_.invalidate_cache_range(entry, 12U);
    return true;
}

} // namespace ilemu
