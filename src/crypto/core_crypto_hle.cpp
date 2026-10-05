// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Accelerate supported guest corecrypto big-integer operations with
// firmware fallback.

#include "crypto/core_crypto_hle.hpp"

#include "foundation/address_space.hpp"
#include "crypto/big_number.hpp"
#include "foundation/userland_hle.hpp"
#include "prime_field_layout.hpp"
#include <string>
#include <vector>

namespace ilemu {
namespace {

    void common_multiple_or_divisor(UserlandHleCall& call, bool multiple)
    {
        // ccn uses fixed-width, little-endian ARM32 units, including leading
        // zero units. Read both operands before writing to allow output aliasing.
        const auto units = call.argument(0);
        constexpr std::uint32_t maximum_units = 512U;
        if (units == 0U || units > maximum_units) {
            call.resume_original_persistently();
            return;
        }
        const auto size =
            static_cast<std::size_t>(units) * sizeof(std::uint32_t);
        const auto first = call.memory().read_bytes(call.argument(2), size);
        const auto second = call.memory().read_bytes(call.argument(3), size);
        const auto result_size = size * (multiple ? 2U : 1U);
        const auto destination = call.argument(1);
        if (!first || !second || !call.memory().accessible(
                                    destination, result_size, MemoryPermission::Write)) {
            call.resume_original_persistently();
            return;
        }
        std::vector<std::byte> result(result_size);
        const auto operation = multiple
            ? BigNumberArithmetic::least_common_multiple
            : BigNumberArithmetic::greatest_common_divisor;
        if (!operation(*first, *second, result) ||
            !call.memory().copy_in(destination, result)) {
            call.resume_original_persistently();
            return;
        }
        call.set_return(0U);
    }

    void greatest_common_divisor(UserlandHleCall& call)
    {
        common_multiple_or_divisor(call, false);
    }

    void least_common_multiple(UserlandHleCall& call)
    {
        // ccn_lcm(n, r2n, s, t): output contains 2*n ARM32 units.
        common_multiple_or_divisor(call, true);
    }

    void power_modulo_arguments(UserlandHleCall& call, std::size_t first)
    {
        const auto context = call.argument(first);
        const auto destination = call.argument(first + 1U);
        const auto reduction = call.symbol_address("_cczp_mod");
        const auto profile = reduction ? PrimeFieldLayout::resolve(
                                             call.memory(), context, *reduction)
                                       : std::nullopt;
        const auto units = call.memory().read32(context);
        // Bound host allocation and work. Larger operands retain the original
        // firmware path, including its own allocation and error semantics.
        constexpr std::uint32_t maximum_units = 512U;
        if (!profile || !units || *units == 0U || *units > maximum_units) {
            call.resume_original_persistently();
            return;
        }
        const auto size =
            static_cast<std::size_t>(*units) * sizeof(std::uint32_t);
        const auto base = call.memory().read_bytes(call.argument(first + 2U), size);
        const auto exponent =
            call.memory().read_bytes(call.argument(first + 3U), size);
        const auto modulus =
            call.memory().read_bytes(context + profile->modulus_offset, size);
        if (!base || !exponent || !modulus ||
            !call.memory().accessible(
                destination, size, MemoryPermission::Write)) {
            call.resume_original_persistently();
            return;
        }
        std::vector<std::byte> result(size);
        if (!BigNumberArithmetic::power_modulo(
                *base, *exponent, *modulus, result) ||
            !call.memory().copy_in(destination, result)) {
            call.resume_original_persistently();
            return;
        }
        call.set_return(0U);
    }

    void power_modulo(UserlandHleCall& call)
    {
        power_modulo_arguments(call, 0U);
    }

    void power_modulo_workspace(UserlandHleCall& call)
    {
        // The workspace form prepends a scratch allocator, whose cursor is
        // restored before return. Host arithmetic owns its temporary storage.
        power_modulo_arguments(call, 1U);
    }

} // namespace

void register_core_crypto_hle(UserlandHleRegistry& registry)
{
    for (const auto* image :
        { "/Security.framework/Security", "/libcorecrypto.dylib" }) {
        registry.register_guest_function(image, "_cczp_mod");
        registry.register_function(image, "_cczp_power", power_modulo);
        // Private arithmetic entries can be covered by native code-integrity
        // checks. Accelerate instruction fetches without changing data reads.
        registry.register_function(image, "_cczp_power_ssma", power_modulo,
            UserlandHleRegistry::SymbolLookup::ImageAndCacheLocals,
            UserlandHleRegistry::EntryPatch::InstructionFetch);
        registry.register_function(image, "_cczp_power_ssma_ws",
            power_modulo_workspace,
            UserlandHleRegistry::SymbolLookup::ImageAndCacheLocals,
            UserlandHleRegistry::EntryPatch::InstructionFetch);
        registry.register_function(image, "_ccn_lcm", least_common_multiple,
            UserlandHleRegistry::SymbolLookup::ImageAndCacheLocals,
            UserlandHleRegistry::EntryPatch::InstructionFetch);
        registry.register_function(image, "_ccn_gcd", greatest_common_divisor,
            UserlandHleRegistry::SymbolLookup::ImageAndCacheLocals,
            UserlandHleRegistry::EntryPatch::InstructionFetch);
    }
}

} // namespace ilemu
