#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ilemu::darwin::resource {

inline constexpr std::uint32_t cpu = 0;
inline constexpr std::uint32_t file_size = 1;
inline constexpr std::uint32_t data = 2;
inline constexpr std::uint32_t stack = 3;
inline constexpr std::uint32_t core = 4;
inline constexpr std::uint32_t address_space = 5;
inline constexpr std::uint32_t locked_memory = 6;
inline constexpr std::uint32_t process_count = 7;
inline constexpr std::uint32_t open_files = 8;
inline constexpr std::size_t limit_count = 9;
// Darwin's UNIX03 libc wrappers set this private bit before entering the
// kernel. xnu-1228 strips it in getrlimit/dosetrlimit after retaining whether
// strict POSIX error behavior was requested.
inline constexpr std::uint32_t posix_flag = 0x1000;

[[nodiscard]] constexpr std::uint32_t selector(std::uint32_t raw)
{
    return raw & ~posix_flag;
}

[[nodiscard]] constexpr bool requests_posix_behavior(std::uint32_t raw)
{
    return (raw & posix_flag) != 0;
}

inline constexpr std::uint64_t infinity = (std::uint64_t { 1 } << 63U) - 1U;
inline constexpr std::uint64_t initial_open_files = 256;
inline constexpr std::uint64_t maximum_open_files = 10'240;
inline constexpr std::size_t arm32_limit_size = 16;

struct Limit {
    std::uint64_t current { infinity };
    std::uint64_t maximum { infinity };
};

[[nodiscard]] constexpr std::array<Limit, limit_count> initial_limits()
{
    std::array<Limit, limit_count> limits { };
    limits[open_files] = Limit { initial_open_files, maximum_open_files };
    return limits;
}

} // namespace ilemu::darwin::resource
