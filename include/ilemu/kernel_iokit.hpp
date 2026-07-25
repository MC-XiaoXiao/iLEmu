#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

class AddressSpace;
class Output;
struct KernelSharedState;
struct ProcessContext;

struct IOKitMachCallSite {
  std::uint32_t program_counter{};
  std::uint32_t link_register{};
  std::uint32_t frame_pointer{};
};

// Handles the device/iokit.defs MIG subset used by the iPhoneOS 1.0
// userspace.  A value is returned when the message ID belongs to this module;
// std::nullopt leaves the request available to another Mach subsystem.
[[nodiscard]] std::optional<std::uint32_t> handle_iokit_mach_request(
    AddressSpace& memory,
    Output& output,
    KernelSharedState& shared_state,
    ProcessContext& process,
    std::uint32_t message_id,
    std::uint32_t message_address,
    std::uint32_t send_size,
    std::uint32_t receive_size,
    std::uint32_t remote_port,
    std::uint32_t local_port,
    IOKitMachCallSite call_site = {});

}  // namespace ilemu
