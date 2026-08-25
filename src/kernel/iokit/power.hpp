#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

class AddressSpace;
class Output;
struct KernelSharedState;
struct ProcessContext;

namespace kernel_iokit {

    // Implements the root power-domain subset used by
    // IORegisterForSystemPower. Requests for other IOKit services are left to
    // the general device dispatcher.
    [[nodiscard]] std::optional<std::uint32_t> handle_power_mach_request(
        AddressSpace& memory, Output& output, KernelSharedState& shared_state,
        ProcessContext& process, std::uint32_t message_id,
        std::uint32_t message_address, std::uint32_t receive_size,
        std::uint32_t remote_object, std::uint32_t local_port);

} // namespace kernel_iokit
} // namespace ilemu
