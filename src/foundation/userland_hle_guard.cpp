// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/userland_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"

#include <algorithm>
#include <array>
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

    void append_halfword(std::vector<std::byte>& code, std::uint32_t halfword)
    {
        code.push_back(static_cast<std::byte>(halfword & 0xffU));
        code.push_back(static_cast<std::byte>((halfword >> 8U) & 0xffU));
    }

    bool relocatable_arm_frame(std::span<const std::byte> code)
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

    std::optional<std::uint32_t> thumb_immediate(std::uint32_t value)
    {
        if (value <= 0xffU)
            return value;
        for (unsigned rotation = 8; rotation < 32U; ++rotation) {
            const auto byte = std::rotl(value, static_cast<int>(rotation));
            if (byte >= 0x80U && byte <= 0xffU)
                return (rotation << 7U) | (byte & 0x7fU);
        }
        return std::nullopt;
    }

    // Thumb modified and wide immediates use the same i:imm3:imm8 fields.
    void append_thumb_immediate(std::vector<std::byte>& code,
        std::uint32_t first, std::uint32_t second, std::uint32_t immediate)
    {
        append_halfword(code, first | ((immediate & 0x800U) >> 1U));
        append_halfword(
            code, second | ((immediate & 0x700U) << 4U) | (immediate & 0xffU));
    }

    struct ArgumentGuard {
        std::vector<std::byte> trampoline;
        std::vector<std::byte> patch;
        std::uint32_t svc_offset;
        std::uint32_t original_offset;
    };

    std::optional<ArgumentGuard> make_arm_guard(
        std::span<const std::byte> original, std::uint32_t entry,
        std::uint32_t address, std::uint32_t index, std::uint32_t minimum,
        std::uint32_t maximum, std::uint16_t id)
    {
        const auto low = arm_immediate(minimum);
        const auto high = arm_immediate(maximum);
        if (!low || !high || !relocatable_arm_frame(original))
            return std::nullopt;

        ArgumentGuard guard { { }, { }, 16U, 20U };
        auto& code = guard.trampoline;
        append_word(code, 0xe3500000U | (index << 16U) | *low);
        append_word(code, 0x3a000002U); // blo original prologue
        append_word(code, 0xe3500000U | (index << 16U) | *high);
        append_word(code, 0x8a000000U); // bhi original prologue
        append_word(code, 0xef000000U | userland_hle_svc_namespace | id);
        code.insert(code.end(), original.begin(), original.end());
        append_word(code, 0xe51ff004U); // ldr pc,[pc,#-4]
        append_word(code, entry + 12U);

        append_word(guard.patch, 0xe300c000U | ((address & 0xf000U) << 4U) |
                                     (address & 0xfffU)); // movw ip,lo16
        append_word(
            guard.patch, 0xe340c000U | ((address & 0xf0000000U) >> 12U) |
                             ((address >> 16U) & 0xfffU)); // movt ip,hi16
        append_word(guard.patch, 0xe12fff1cU); // bx ip
        return guard;
    }

    void append_thumb_prologue_return(
        std::vector<std::byte>& code, std::uint32_t entry)
    {
        append_halfword(code, 0xf8dfU); // ldr.w pc,[pc,#4]
        append_halfword(code, 0xf004U);
        append_halfword(code, 0xbf00U); // align the Thumb branch target literal
        append_word(code, (entry + 10U) | 1U);
    }

    ArgumentGuard make_thumb_move_guard(std::span<const std::byte> original,
        std::uint32_t entry, std::uint32_t index, std::uint32_t low,
        std::uint32_t high, std::uint32_t return_offset = 0x96U,
        bool wide_equal_branch = false)
    {
        ArgumentGuard guard { { }, { }, 16U, 20U };
        auto& code = guard.trampoline;
        append_thumb_immediate(code, 0xf1b0U | index, 0x0f00U, low); // cmp.w
        append_halfword(code, 0xd306U); // blo original prologue
        append_thumb_immediate(code, 0xf1b0U | index, 0x0f00U, high);
        append_halfword(code, 0xd803U); // bhi original prologue
        // Keep a same-address move on the firmware's immediate-return path.
        append_halfword(code, 0x4288U); // cmp r0,r1
        append_halfword(code, 0xd001U); // beq original prologue
        append_halfword(code, 0xdf00U | userland_hle_thumb_svc);
        append_halfword(code, 0xbf00U); // align original prologue
        code.insert(code.end(), original.begin(), original.end());
        code[guard.original_offset + 6U] =
            std::byte { 0x05 }; // beq second ldr.w
        code[guard.original_offset + 7U] = std::byte { 0xd0 };
        if (wide_equal_branch) {
            // Replace the firmware's four-byte BEQ.W with BEQ + NOP. The
            // relocated equal path reaches its original pop {r7,pc}; the
            // unequal path resumes at entry+10, which assigns IP from r0.
            code[guard.original_offset + 8U] = std::byte { 0x00 };
            code[guard.original_offset + 9U] = std::byte { 0xbf };
        }
        append_thumb_prologue_return(code, entry);
        append_halfword(code, 0xf8dfU); // ldr.w pc,[pc]
        append_halfword(code, 0xf000U);
        append_word(code, (entry + return_offset) | 1U);
        return guard;
    }

    ArgumentGuard make_thumb_unicode_guard(std::span<const std::byte> original,
        std::uint32_t entry, std::uint32_t low, std::uint32_t high)
    {
        ArgumentGuard guard { { }, { }, 40U, 52U };
        auto& code = guard.trampoline;
        append_thumb_immediate(code, 0xf1b2U, 0x0f00U, low); // cmp.w r2
        append_halfword(code, 0xd316U); // blo original prologue
        append_thumb_immediate(code, 0xf1b2U, 0x0f00U, high);
        append_halfword(code, 0xd813U); // bhi original prologue
        append_halfword(code, 0xf401U); // and.w r3,r1,#0xf800
        append_halfword(code, 0x4378U);
        append_halfword(code, 0xf5b3U); // cmp.w r3,#0xd800
        append_halfword(code, 0x4f58U);
        append_halfword(code, 0xd00eU); // beq original prologue
        // Preserve the original frame stores before the first unit load.
        // Only a first-unit mismatch reaches the SVC with the frame live.
        code.insert(code.end(), original.begin(), original.end());
        append_halfword(code, 0x0052U); // lsls r2,r2,#1
        append_halfword(code, 0x8803U); // ldrh r3,[r0]
        append_halfword(code, 0x428bU); // cmp r3,r1
        append_halfword(code, 0xd001U); // beq original epilogue
        append_halfword(code, 0xdf00U | userland_hle_thumb_svc);
        append_halfword(code, 0xbf00U); // align epilogue veneer
        append_halfword(code, 0xf8dfU); // ldr.w pc,[pc]
        append_halfword(code, 0xf000U);
        append_word(code, (entry + 56U) | 1U);
        code.insert(code.end(), original.begin(), original.end());
        append_thumb_prologue_return(code, entry);
        return guard;
    }

    std::optional<ArgumentGuard> make_thumb_guard(
        std::span<const std::byte> original, std::uint32_t entry,
        std::uint32_t address, std::uint32_t index, std::uint32_t minimum,
        std::uint32_t maximum)
    {
        const auto low = thumb_immediate(minimum);
        const auto high = thumb_immediate(maximum);
        if (!low || !high)
            return std::nullopt;

        const auto matches = [original](
                                 const std::array<std::uint8_t, 10>& bytes) {
            return std::equal(original.begin(), original.end(), bytes.begin(),
                bytes.end(), [](std::byte a, std::uint8_t b) {
                    return std::to_integer<std::uint8_t>(a) == b;
                });
        };
        ArgumentGuard guard;
        // push {r7,lr}; mov r7,sp; subs r3,r0,r1; beq entry+0x96; mov ip,r0
        if (matches({ 0x80, 0xb5, 0x6f, 0x46, 0x43, 0x1a, 0x46, 0xd0, 0x84,
                0x46 })) {
            guard = make_thumb_move_guard(original, entry, index, *low, *high);
            // push {r7,lr}; mov r7,sp; subs r3,r0,r1; beq.w entry+0x2a8
        } else if (matches({ 0x80, 0xb5, 0x6f, 0x46, 0x43, 0x1a, 0x00, 0xf0,
                       0x4f, 0x81 })) {
            guard = make_thumb_move_guard(
                original, entry, index, *low, *high, 0x2a8U, true);
            // push {r7,lr}; mov r7,sp; sub sp,#4; strh r1,[r7,#-2]
        } else if (index == 2U && matches({ 0x80, 0xb5, 0x6f, 0x46, 0x81, 0xb0,
                                      0x27, 0xf8, 0x02, 0x1c })) {
            // This guard executes the firmware's first-unit match itself.
            // A write anywhere in that routine must drop the entry overlay.
            if (entry % AddressSpace::page_size > AddressSpace::page_size - 60U)
                return std::nullopt;
            guard = make_thumb_unicode_guard(original, entry, *low, *high);
        } else {
            return std::nullopt;
        }

        const auto low_target = (address & 0xffffU) | 1U;
        const auto high_target = address >> 16U;
        append_thumb_immediate(guard.patch, 0xf240U | (low_target >> 12U),
            0x0c00U, low_target); // movw ip,lo16
        append_thumb_immediate(guard.patch, 0xf2c0U | (high_target >> 12U),
            0x0c00U, high_target); // movt ip,hi16
        append_halfword(guard.patch, 0x4760U); // bx ip
        return guard;
    }

} // namespace

std::optional<std::uint32_t> UserlandHleCall::argument_guard_entry() const
{
    const auto call = registry_.installed_calls_.find(entry_address_);
    if (call == registry_.installed_calls_.end() ||
        !call->second.original_entry)
        return std::nullopt;
    return call->second.original_entry;
}

bool UserlandHleCall::guard_unsigned_argument(
    std::size_t index, std::uint32_t minimum, std::uint32_t maximum,
    const FirmwareRoutine& routine)
{
    const AddressSpace::ExclusiveAccess access { memory_ };
    const auto entry = entry_address_;
    const auto installed = registry_.installed_calls_.find(entry);
    if (installed == registry_.installed_calls_.end() ||
        installed->second.prefix_arguments != 0U || index >= 4U ||
        minimum > maximum)
        return false;
    if (installed->second.original_entry != 0U)
        return routine.matches(memory_, installed->second.original_entry);
    if (!routine.matches(memory_, entry))
        return false;
    const auto* registration =
        registry_.find_registration(installed->second.id);
    const auto thumb = installed->second.thumb;
    const auto instruction_size = thumb ? 2U : 4U;
    const auto patch_size = thumb ? 10U : 12U;
    if (!registration ||
        registration->patch !=
            UserlandHleRegistry::EntryPatch::InstructionFetch ||
        installed->second.original.size() != instruction_size)
        return false;
    for (auto offset = instruction_size; offset < patch_size;
        offset += instruction_size) {
        if (registry_.installed_calls_.contains(entry + offset))
            return false;
    }
    auto original = memory_.read_bytes(entry, patch_size);
    if (!original || !std::equal(installed->second.original.begin(),
                         installed->second.original.end(), original->begin()))
        return false;

    const auto address = (registry_.persistent_trampoline_cursor_ + 3U) & ~3U;
    const auto register_index = static_cast<std::uint32_t>(index);
    auto guard = thumb
                     ? make_thumb_guard(*original, entry, address,
                           register_index, minimum, maximum)
                     : make_arm_guard(*original, entry, address, register_index,
                           minimum, maximum, installed->second.id);
    if (!guard)
        return false;
    const auto code_size = static_cast<std::uint32_t>(guard->trampoline.size());
    if (address > 0x61000000U - code_size)
        return false;
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
    if (!memory_.copy_in(address, guard->trampoline))
        return false;
    auto guarded = installed->second;
    guarded.original = *original;
    guarded.original_entry = entry;
    const auto [alias, inserted] = registry_.installed_calls_.emplace(
        address + guard->svc_offset, std::move(guarded));
    if (!inserted)
        return false;
    // Instruction overlays are deliberately invisible to data loads. Build
    // the absolute target in volatile IP before transferring control.
    const AddressSpace::CopyInOperation operation { entry, guard->patch };
    if (!memory_.overlay_instructions({ &operation, 1U })) {
        registry_.installed_calls_.erase(alias);
        return false;
    }
    // Preserve the source symbol's storage: the active call has a string_view
    // into it. Map insertion leaves the dispatcher's iterator valid as well.
    installed->second.original = std::move(*original);
    installed->second.original_entry = entry;
    installed->second.argument_guard_patch =
        std::make_shared<const UserlandHleRegistry::ArgumentGuardPatch>(
            routine, std::move(guard->patch));
    registry_.persistent_trampolines_.insert_or_assign(
        entry, address + guard->original_offset);
    registry_.persistent_trampoline_cursor_ = address + code_size;
    cpu_.invalidate_cache_range(address, code_size);
    cpu_.invalidate_cache_range(entry, patch_size);
    return true;
}

} // namespace ilemu
