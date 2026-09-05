#pragma once

#include <cstdint>

namespace ilemu {

class AddressSpace;

namespace bsd::sandbox {

enum class CallResult {
    Unsupported,
    Success,
    BadAddress,
};

[[nodiscard]] CallResult dispatch(
    AddressSpace& memory, std::uint32_t operation, std::uint32_t argument);

} // namespace bsd::sandbox
} // namespace ilemu
