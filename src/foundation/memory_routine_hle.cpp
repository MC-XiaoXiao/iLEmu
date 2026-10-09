// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/memory_routine_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/content_identity.hpp"
#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ilemu {
namespace {

    // ARM AAPCS memset: r0=destination, r1=byte, r2=size, r0 returned.
    // This position-independent NEON implementation has 64-byte, 8-byte and
    // byte tails, all contained in these 46 ARM instructions. The digest
    // covers every instruction and return, not just an entry prologue.
    // It identifies executable semantics independently of device/OS versions.
    struct NeonMemoryFill {
        static constexpr std::size_t code_size = 184U;
        static constexpr std::array<std::uint8_t, 32> digest {
            0x15,
            0x10,
            0x1c,
            0x82,
            0xfd,
            0x09,
            0xe6,
            0x5e,
            0x0b,
            0x9b,
            0xcc,
            0xb0,
            0x4c,
            0xdf,
            0xd1,
            0x58,
            0xb5,
            0x95,
            0x0e,
            0x2e,
            0x20,
            0xac,
            0x2f,
            0x49,
            0xa3,
            0x99,
            0x6a,
            0xaa,
            0x75,
            0x00,
            0x73,
            0x34,
        };

        [[nodiscard]] static bool matches(
            AddressSpace& memory, std::uint32_t entry)
        {
            const auto code = memory.read_bytes(entry, code_size);
            if (!code)
                return false;
            const auto identity = sha256(*code);
            for (std::size_t i = 0; i < digest.size(); ++i) {
                if (std::to_integer<std::uint8_t>(identity.digest[i]) !=
                    digest[i])
                    return false;
            }
            return true;
        }
    };

    class MemoryFillAccelerator {
    public:
        void operator()(UserlandHleCall& call) const
        {
            // Preserve instruction breakpoints and per-store watchpoints.
            if (call.cpu().guest_debugging_enabled()) {
                call.resume_original();
                return;
            }
            auto& memory = call.memory();
            const AddressSpace::ExclusiveAccess access { memory };
            const auto entry = call.symbol_address(call.symbol());
            if (!entry || (call.cpu().cpsr() & (1U << 5U)) != 0U) {
                call.resume_original();
                return;
            }
            if (!NeonMemoryFill::matches(memory, *entry)) {
                call.resume_original();
                return;
            }

            if (!call.guard_unsigned_argument(
                    2U, minimum_host_size, maximum_host_size)) {
                call.resume_original();
                return;
            }

            const auto destination = call.argument(0);
            const auto size = call.argument(2);
            // Keep host work bounded and small operations in the firmware.
            // Validate every mapping before the first write. In particular,
            // executable output must retain ordinary guest invalidation.
            if (size < minimum_host_size || size > maximum_host_size ||
                static_cast<std::uint64_t>(destination) + size >
                    (1ULL << 32U) ||
                !writable_data(memory, destination, size)) {
                call.resume_original_persistently();
                return;
            }
            const std::vector<std::byte> bytes(
                size, static_cast<std::byte>(call.argument(1) & 0xffU));
            if (!memory.copy_to_user(destination, bytes)) {
                call.resume_original_persistently();
                return;
            }
            call.set_return(destination);
        }

    private:
        // A page-sized floor amortizes dispatch and code verification; bound
        // temporary host memory and let larger fills retain guest scheduling.
        static constexpr std::uint32_t minimum_host_size = 4096U;
        static constexpr std::uint32_t maximum_host_size = 1024U * 1024U;

        [[nodiscard]] static bool writable_data(
            AddressSpace& memory, std::uint32_t address, std::uint32_t size)
        {
            const auto end = static_cast<std::uint64_t>(address) + size;
            for (std::uint64_t cursor = address; cursor < end;) {
                const auto region = memory.mapping_region_at_or_after(
                    static_cast<std::uint32_t>(cursor));
                if (!region || region->address > cursor ||
                    !has_permission(
                        region->permissions, MemoryPermission::Write) ||
                    has_permission(
                        region->permissions, MemoryPermission::Execute))
                    return false;
                cursor = std::min(end, region->end);
            }
            return true;
        }
    };

} // namespace

void register_memory_routine_hle(
    UserlandHleRegistry& registry, ArmArchitectureVersion architecture)
{
    if (architecture != ArmArchitectureVersion::Armv7)
        return;
    const UserlandHleRegistry::Handler handler = MemoryFillAccelerator { };
    // Resolve implementation variants from symbols, including cache locals.
    // Unknown implementations resume their original first instruction and
    // remove the interception. Instruction overlays leave firmware bytes
    // intact.
    registry.register_prefix("/libsystem_c.dylib", "_memset$VARIANT$", handler,
        UserlandHleRegistry::SymbolLookup::ImageAndCacheLocals,
        UserlandHleRegistry::EntryPatch::InstructionFetch);
    registry.register_function("/libsystem_c.dylib", "_memset", handler,
        UserlandHleRegistry::SymbolLookup::Image,
        UserlandHleRegistry::EntryPatch::InstructionFetch);
}

} // namespace ilemu
