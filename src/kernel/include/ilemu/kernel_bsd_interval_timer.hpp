#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

class AddressSpace;
class Cpu;
class Output;
struct KernelSharedState;
struct ProcessContext;

namespace kernel_bsd::interval_timer {

inline constexpr std::uint32_t set_syscall = 83;
inline constexpr std::uint32_t get_syscall = 86;
inline constexpr std::uint32_t expiration_signal = 14; // SIGALRM

[[nodiscard]] bool dispatch(Cpu &cpu, AddressSpace &memory, Output &output,
                            KernelSharedState &state,
                            const ProcessContext &process,
                            std::uint32_t syscall_number);

[[nodiscard]] std::optional<std::uint64_t>
next_deadline(KernelSharedState &state, std::uint32_t process_id);

// Advances one process's ITIMER_REAL state and reports whether SIGALRM should
// be delivered. Repeating expirations coalesce like ordinary process signals.
[[nodiscard]] bool service_due(KernelSharedState &state,
                               std::uint32_t process_id,
                               std::uint64_t serviced_deadline);

void retire_process(KernelSharedState &state, std::uint32_t process_id);

} // namespace kernel_bsd::interval_timer
} // namespace ilemu
