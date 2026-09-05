#pragma once

#include <cstdint>

namespace ilemu::darwin::proc_info {

// Darwin 9 introduced the private __proc_info syscall used by libproc.
inline constexpr std::uint32_t syscall_number = 336U;

inline constexpr std::uint32_t call_pid_info = 2U;
inline constexpr std::uint32_t flavor_pid_path_info = 11U;

// PROC_PIDPATHINFO accepts one to four MAXPATHLEN buffers. The kernel clears
// and copies out the complete caller-provided range, not only the string.
inline constexpr std::uint32_t path_info_size = 1024U;
inline constexpr std::uint32_t path_info_max_size = 4U * path_info_size;

} // namespace ilemu::darwin::proc_info
