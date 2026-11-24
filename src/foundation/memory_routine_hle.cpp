// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/memory_routine_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/firmware_routine.hpp"
#include "foundation/userland_hle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ilemu {
namespace {

    // ARM memset: r0=destination, r1=byte, r2=size, r0 returned. The entire
    // NEON routine is position-independent and shared by older libsystem_c
    // and libsystem_platform. bzero sets r2=size and r1=0 before falling in.
    constexpr FirmwareRoutine neon_memory_fill { 184U,
        { 0x15, 0x10, 0x1c, 0x82, 0xfd, 0x09, 0xe6, 0x5e, 0x0b, 0x9b, 0xcc,
            0xb0, 0x4c, 0xdf, 0xd1, 0x58, 0xb5, 0x95, 0x0e, 0x2e, 0x20, 0xac,
            0x2f, 0x49, 0xa3, 0x99, 0x6a, 0xaa, 0x75, 0x00, 0x73, 0x34 } };

    // The A6 CPU's Thumb-2 memmove serves both memcpy and memmove in the ARMv7s
    // shared cache. r0=destination, r1=source, r2=size, r0 returned.
    // The fingerprint includes the forward/backward paths and jump tables.
    constexpr FirmwareRoutine neon_memory_move { 0x610U,
        { 0xe0, 0xad, 0xdf, 0x76, 0x78, 0xc5, 0xb8, 0x35, 0xff, 0x00, 0x1f,
            0x77, 0xa9, 0x8e, 0x27, 0x93, 0x0c, 0x49, 0xf3, 0x79, 0x6c, 0x28,
            0xea, 0xef, 0xf4, 0xe5, 0x94, 0x71, 0x1d, 0x60, 0xdf, 0x4c } };

    enum class MemoryRoutine { LegacyFill, PlatformFill, PlatformMove };

    class MemoryRoutineAccelerator {
    public:
        explicit MemoryRoutineAccelerator(MemoryRoutine routine)
            : routine_ { routine }
        {
        }

        void operator()(UserlandHleCall& call) const
        {
            if (call.cpu().guest_debugging_enabled()) {
                call.resume_original();
                return;
            }

            auto& memory = call.memory();
            const AddressSpace::ExclusiveAccess access { memory };
            const auto cpsr = call.cpu().cpsr();
            const auto thumb = (cpsr & (1U << 5U)) != 0U;
            const bool move = routine_ == MemoryRoutine::PlatformMove;
            const bool legacy_fill = routine_ == MemoryRoutine::LegacyFill;
            const auto& fingerprint =
                move ? neon_memory_move : neon_memory_fill;
            const auto minimum_host_size = move ? 32U * 1024U
                                           : legacy_fill
                                               ? AddressSpace::page_size
                                               : 64U * 1024U;
            if (thumb != move ||
                !call.guard_unsigned_argument(
                    2U, minimum_host_size, maximum_host_size, fingerprint)) {
                call.resume_original();
                return;
            }

            const auto destination = call.argument(0);
            const auto source_or_byte = call.argument(1);
            const auto size = call.argument(2);
            const auto& registers = call.cpu().registers();
            const auto sp = registers[13];
            const bool big_endian = (cpsr & (1U << 9U)) != 0U;
            if (big_endian || size < minimum_host_size ||
                size > maximum_host_size || !valid_range(destination, size) ||
                (sp & 3U) != 0U || sp < 8U ||
                overlaps_frame(destination, size, sp) ||
                (move && (destination == source_or_byte ||
                             !valid_range(source_or_byte, size) ||
                             overlaps_frame(source_or_byte, size, sp)))) {
                call.resume_original_persistently();
                return;
            }

            // Reproduce the two stack stores from push {r7,lr}. They must
            // pass preflight together with the destination so we can still
            // execute the guest routine if any store would fail.
            std::array<std::byte, 8> frame { };
            for (std::size_t i = 0; i < 4U; ++i) {
                frame[i] = static_cast<std::byte>(registers[7] >> (8U * i));
                frame[i + 4U] =
                    static_cast<std::byte>(registers[14] >> (8U * i));
            }
            const AddressSpace::CopyInOperation store { sp - 8U, frame };
            const auto fill_byte =
                static_cast<std::byte>(source_or_byte & 0xffU);
            auto completed = move ? memory.try_move_private_data(destination,
                                        source_or_byte, size, store)
                                  : memory.try_fill_private_data(
                                        destination, fill_byte, size, store);
            if (!completed && legacy_fill &&
                memory.accessible(sp - 8U, frame.size(), MemoryPermission::Read)) {
                // Keep older libsystem_c's checked fill path for private
                // demand-zero pages and COW destinations.
                const std::vector<std::byte> bytes(size, fill_byte);
                const std::array operations { store,
                    AddressSpace::CopyInOperation { destination, bytes } };
                completed = memory.try_copy_to_private_data(operations);
            }
            if (!completed) {
                call.resume_original_persistently();
                return;
            }
            call.set_return(destination);
        }

    private:
        // Bound host work and leave unusually large operations interruptible.
        static constexpr std::uint32_t maximum_host_size = 1024U * 1024U;

        [[nodiscard]] static bool valid_range(
            std::uint32_t address, std::uint32_t size)
        {
            return static_cast<std::uint64_t>(address) + size <= (1ULL << 32U);
        }

        [[nodiscard]] static bool overlaps_frame(
            std::uint32_t address, std::uint32_t size, std::uint32_t sp)
        {
            const auto end = static_cast<std::uint64_t>(address) + size;
            return address < sp && end > sp - 8U;
        }

        MemoryRoutine routine_;
    };

} // namespace

void register_memory_routine_hle(
    UserlandHleRegistry& registry, ArmArchitectureVersion architecture)
{
    if (architecture != ArmArchitectureVersion::Armv7)
        return;
    using Lookup = UserlandHleRegistry::SymbolLookup;
    constexpr auto overlay = UserlandHleRegistry::EntryPatch::InstructionFetch;
    const UserlandHleRegistry::Handler legacy_fill =
        MemoryRoutineAccelerator { MemoryRoutine::LegacyFill };
    registry.register_prefix("/libsystem_c.dylib", "_memset$VARIANT$",
        legacy_fill, Lookup::ImageAndCacheLocals, overlay);
    registry.register_function(
        "/libsystem_c.dylib", "_memset", legacy_fill, Lookup::Image, overlay);
    registry.register_function("/libsystem_platform.dylib", "__platform_memset",
        MemoryRoutineAccelerator { MemoryRoutine::PlatformFill }, Lookup::Image,
        overlay);
    registry.register_function("/libsystem_platform.dylib",
        "__platform_memmove",
        MemoryRoutineAccelerator { MemoryRoutine::PlatformMove }, Lookup::Image,
        overlay);
}

} // namespace ilemu
