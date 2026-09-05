#include "prime_field_profile.hpp"

#include "ilemu/address_space.hpp"
#include <limits>

namespace ilemu {

std::optional<PrimeFieldProfile> PrimeFieldProfile::resolve(
    const AddressSpace& memory, std::uint32_t context,
    std::uint32_t standard_reduction)
{
    if (context > std::numeric_limits<std::uint32_t>::max() - 8U ||
        standard_reduction == 0U) {
        return std::nullopt;
    }
    constexpr PrimeFieldProfile compact_arm32 { 8U };
    const auto reduction = memory.read32(context + sizeof(std::uint32_t));
    if (reduction && (*reduction & ~1U) == (standard_reduction & ~1U))
        return compact_arm32;
    return std::nullopt;
}

} // namespace ilemu
