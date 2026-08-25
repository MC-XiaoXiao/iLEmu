#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/vm_map_mig_ids.hpp"

#include <array>
#include <cstdint>
#include <string>

#include "../support.hpp"
#include "wire_reply.hpp"

namespace ilemu {
namespace {

    // XNU 792 publishes the pointer-sized vm_map interface at 3800 and also
    // exposes its mach_vm compatibility subsystem at 4800. On ARM32 the
    // mach_vm_allocate client uses the same 32-bit wire fields as vm_allocate.
    constexpr std::uint32_t mach_vm_allocate_identifier = 4800U;
    constexpr std::uint32_t request_size = 44U;
    constexpr std::uint32_t reply_size = 40U;

} // namespace

bool CompatibilityKernel::dispatch_mach_vm_allocate_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    using namespace mach_support;
    using namespace mach_vm_support;
    const auto vm_allocate_identifier =
        mig_message_id(xnu792::mig::vm_map::Routine::vm_allocate);
    if (request.identifier != vm_allocate_identifier &&
        request.identifier != mach_vm_allocate_identifier) {
        return false;
    }

    auto& registers = cpu.registers();
    if (registers[2] < request_size || registers[3] < reply_size) {
        registers[0] = mach_receive_invalid_data;
        return true;
    }

    const auto& arguments = xnu792::mig::vm_map::vm_allocate_arguments;
    auto address = memory_.read32(request.address + arguments[1].request_offset)
                       .value_or(0);
    const auto requested_address = address;
    const auto size =
        memory_.read32(request.address + arguments[2].request_offset)
            .value_or(0);
    const auto flags =
        memory_.read32(request.address + arguments[3].request_offset)
            .value_or(0);
    if ((flags & darwin::mach::vm_flags_anywhere) != 0) {
        address = find_free_guest_region(memory_, default_dynamic_base, size)
                      .value_or(0);
    }
    const auto mapped = address != 0 && size != 0 &&
                        !guest_region_overlaps(memory_, address, size) &&
                        memory_.map(address, size,
                            MemoryPermission::Read | MemoryPermission::Write);

    const std::array<std::uint32_t, reply_size / sizeof(std::uint32_t)> reply {
        darwin::mig_wire::message_bits(
            darwin::mig_wire::disposition_move_send_once),
        reply_size,
        request.local_port,
        0,
        0,
        request.identifier + 100U,
        0,
        1,
        mapped ? kern_success : 3U, // KERN_NO_SPACE
        address,
    };
    if (!write_words(memory_, request.address, reply)) {
        registers[0] = mach_receive_invalid_data;
        return true;
    }
    output_.write(
        "[vm] allocate pid=" + std::to_string(process_.pid) + " interface=" +
        (request.identifier == mach_vm_allocate_identifier
                ? std::string { "mach_vm" }
                : std::string { "vm_map" }) +
        " requested=" + std::to_string(requested_address) +
        " address=" + std::to_string(address) +
        " size=" + std::to_string(size) + " flags=" + std::to_string(flags) +
        " result=" + std::to_string(mapped ? kern_success : 3U) + "\n");
    registers[0] = kern_success;
    return true;
}

} // namespace ilemu
