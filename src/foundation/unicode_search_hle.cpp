// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/unicode_search_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/firmware_routine.hpp"
#include "foundation/userland_hle.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ilemu {
namespace {

    constexpr std::uint32_t minimum_units = 256U;
    constexpr std::uint32_t maximum_units = 65536U;

    constexpr FirmwareRoutine unicode_search { 60U,
        { 0x7d, 0x72, 0x05, 0xe8, 0x83, 0xd4, 0x42, 0x61, 0xe1, 0x86, 0x98,
            0x8d, 0xee, 0x27, 0x2a, 0xef, 0x32, 0x51, 0xcb, 0xb5, 0xe9, 0x34,
            0x60, 0x4c, 0xb4, 0x80, 0x7d, 0x1e, 0xf4, 0xd0, 0x8d, 0x60 } };

    void search_unicode(UserlandHleCall& call)
    {
        constexpr std::uint32_t thumb_state = 1U << 5U;
        constexpr std::uint32_t big_endian = 1U << 9U;
        auto& memory = call.memory();
        const AddressSpace::ExclusiveAccess access { memory };
        const auto guarded_entry = call.argument_guard_entry();
        const bool guest_only =
            call.cpu().guest_debugging_enabled() ||
            (call.cpu().cpsr() & (thumb_state | big_endian)) != thumb_state;
        if (!guarded_entry) {
            if (guest_only ||
                !call.guard_unsigned_argument(
                    2U, minimum_units, maximum_units, unicode_search)) {
                call.resume_original();
            } else {
                call.resume_original_persistently();
            }
            return;
        }

        // The trampoline has already established the original frame and
        // compared the first unit. The live registers are those of the
        // firmware immediately before its loop increments the pointer.
        const auto advance = (*guarded_entry + 48U) | 1U;
        const auto epilogue = (*guarded_entry + 56U) | 1U;
        if (guest_only || !unicode_search.matches(memory, *guarded_entry)) {
            call.resume_original_at(advance, true);
            return;
        }
        const auto source = call.argument(0);
        const auto unit = call.argument(1);
        const auto size = call.argument(2);
        const auto end = static_cast<std::uint64_t>(source) + size;
        if ((source & 1U) != 0U || unit > 0xffffU || end > (1ULL << 32U)) {
            call.resume_original_at(advance);
            return;
        }

        // Visit pages in guest order. A match on one page must leave later
        // pages untouched, even when the declared span extends across them.
        for (std::uint32_t offset = 2U; offset < size;) {
            const auto address = source + offset;
            const auto chunk = std::min(size - offset,
                AddressSpace::page_size - (address % AddressSpace::page_size));
            const auto bytes = memory.try_read_private_data(address, chunk);
            if (!bytes) {
                call.resume_original_at(advance);
                return;
            }
            for (std::uint32_t i = 0; i < chunk; i += 2U) {
                const auto current =
                    std::to_integer<unsigned>((*bytes)[i]) |
                    (std::to_integer<unsigned>((*bytes)[i + 1U]) << 8U);
                if (current == unit) {
                    call.set_return(address + i);
                    call.resume_original_at(epilogue);
                    return;
                }
            }
            offset += chunk;
        }
        call.set_return(0U);
        call.resume_original_at(epilogue);
    }

} // namespace

void register_unicode_search_hle(
    UserlandHleRegistry& registry, ArmArchitectureVersion architecture)
{
    if (architecture != ArmArchitectureVersion::Armv7)
        return;
    for (const auto* image : { "/libicucore.A.dylib", "/libicucore.dylib" }) {
        registry.register_function(image, "_u_memchr", search_unicode,
            UserlandHleRegistry::SymbolLookup::Image,
            UserlandHleRegistry::EntryPatch::InstructionFetch);
    }
}

} // namespace ilemu
