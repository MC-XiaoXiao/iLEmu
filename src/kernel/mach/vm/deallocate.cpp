#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/vm_map_mig_ids.hpp"

#include <array>
#include <cstdint>

#include "../support.hpp"
#include "wire_reply.hpp"

namespace ilemu {
namespace {

    using namespace mach_support;
    using namespace mach_vm_support;

    // The pointer-sized vm_map subsystem numbers this routine relative to 3800;
    // mach_vm publishes the same ARM32 wire contract relative to 4800.
    constexpr std::uint32_t mach_vm_deallocate_identifier = 4801U;
    constexpr std::uint32_t request_size = 40U;
    constexpr std::uint32_t reply_size = 36U;

} // namespace

bool CompatibilityKernel::dispatch_mach_vm_deallocate_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    const auto vm_deallocate_identifier =
        mig_message_id(xnu792::mig::vm_map::Routine::vm_deallocate);
    const auto is_mach_vm = request.identifier == mach_vm_deallocate_identifier;
    if (request.identifier != vm_deallocate_identifier && !is_mach_vm)
        return false;

    auto& registers = cpu.registers();
    if (registers[2] < request_size || registers[3] < reply_size) {
        registers[0] = mach_receive_invalid_data;
        return true;
    }

    const auto& arguments = xnu792::mig::vm_map::vm_deallocate_arguments;
    const auto address =
        memory_.read32(request.address + arguments[1].request_offset);
    const auto size =
        memory_.read32(request.address + arguments[2].request_offset);

    bool targets_current_task = false;
    {
        std::lock_guard lock { shared_state_->mach_mutex };
        const auto target = target_task_for_port(
            *shared_state_, process_.pid, request.remote_port);
        targets_current_task = target && *target == process_.pid;
    }
    auto result = darwin::mach::invalid_argument;
    if (address && size && targets_current_task) {
        // vm_deallocate treats an already-unmapped subrange as success; unmap
        // whatever currently overlaps the requested page range.
        static_cast<void>(memory_.unmap(*address, *size));
        result = darwin::mach::success;
    }

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
        result,
    };
    if (!write_words(memory_, request.address, reply)) {
        registers[0] = mach_receive_invalid_data;
        return true;
    }

    registers[0] = darwin::mach::success;
    return true;
}

} // namespace ilemu
