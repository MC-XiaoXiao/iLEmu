#pragma once

#include <cstdint>

namespace ilemu {

enum class MemoryPermission : std::uint8_t {
    None = 0,
    Read = 1,
    Write = 2,
    Execute = 4,
};

constexpr MemoryPermission operator|(MemoryPermission lhs, MemoryPermission rhs)
{
    return static_cast<MemoryPermission>(
        static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr MemoryPermission& operator|=(
    MemoryPermission& lhs, MemoryPermission rhs)
{
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool has_permission(MemoryPermission value, MemoryPermission required)
{
    return (static_cast<unsigned>(value) & static_cast<unsigned>(required)) ==
           static_cast<unsigned>(required);
}

} // namespace ilemu
