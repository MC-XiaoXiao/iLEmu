#include "ilemu/core_crypto_hle.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/big_number.hpp"
#include "ilemu/userland_hle.hpp"
#include "prime_field_profile.hpp"
#include <string>
#include <vector>

namespace ilemu {
namespace {

    void power_modulo(UserlandHleCall& call)
    {
        const auto context = call.argument(0);
        const auto destination = call.argument(1);
        const auto reduction = call.symbol_address("_cczp_mod");
        const auto profile = reduction ? PrimeFieldProfile::resolve(
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
        const auto base = call.memory().read_bytes(call.argument(2), size);
        const auto exponent = call.memory().read_bytes(call.argument(3), size);
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

} // namespace

void register_core_crypto_hle(UserlandHleRegistry& registry)
{
    for (const auto* image :
        { "/Security.framework/Security", "/libcorecrypto.dylib" }) {
        registry.register_guest_function(image, "_cczp_mod");
        registry.register_function(image, "_cczp_power", power_modulo);
    }
}

} // namespace ilemu
