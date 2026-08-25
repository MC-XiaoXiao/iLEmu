#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_clock.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/kernel_network.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_host_mig_ids.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/task_mig_ids.hpp"
#include "ilemu/thread_act_mig_ids.hpp"
#include "ilemu/vm_map_mig_ids.hpp"
#include "ilemu/xnu_mig_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {

using namespace mach_support;

bool CompatibilityKernel::dispatch_mach_host_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    auto& registers = cpu.registers();
    const auto message_address = request.address;
    const std::optional<std::uint32_t> bits { request.bits };
    const std::optional<std::uint32_t> remote_port { request.remote_port };
    const std::optional<std::uint32_t> local_port { request.local_port };
    const std::optional<std::uint32_t> message_id { request.identifier };
    if (*message_id ==
            mig_message_id(xnu792::mig::mach_host::Routine::host_info) &&
        registers[3] >= 72) {
        const auto flavor =
            memory_
                .read32(message_address +
                        xnu792::mig::mach_host::host_info_arguments[1]
                            .request_offset)
                .value_or(0);
        const auto requested_count =
            memory_
                .read32(message_address +
                        xnu792::mig::mach_host::host_info_arguments[2]
                            .request_count_offset)
                .value_or(0);
        if (flavor == 1 && requested_count >= 12) { // HOST_BASIC_INFO
            // Darwin 8's host_basic_info is 12 natural_t words. The device CPU
            // and memory identity come from the selected profile; the virtual
            // topology remains an explicit emulator extension for scheduler
            // testing.
            const auto memory_size = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(shared_state_->device_ram_bytes,
                    std::numeric_limits<std::uint32_t>::max()));
            const std::array<std::uint32_t, 22> reply {
                18,
                88,
                *local_port,
                0,
                0,
                *message_id + 100,
                0x00000000U,
                0x00000001U,
                0, // KERN_SUCCESS
                12, // host_info_outCnt
                virtual_processor_count_, // max_cpus
                virtual_processor_count_, // avail_cpus
                memory_size,
                shared_state_->device_cpu_type,
                shared_state_->device_cpu_subtype,
                0, // cpu_threadtype
                virtual_processor_count_, // physical_cpu
                virtual_processor_count_, // physical_cpu_max
                virtual_processor_count_, // logical_cpu
                virtual_processor_count_, // logical_cpu_max
                memory_size, // max_mem, low 32 bits
                0, // max_mem, high 32 bits
            };
            for (std::size_t index = 0; index < reply.size(); ++index) {
                if (!memory_.write32(message_address +
                                         static_cast<std::uint32_t>(index * 4U),
                        reply[index])) {
                    registers[0] = 0x10004008U;
                    return true;
                }
            }
            registers[0] = 0;
            return true;
        }
        if (flavor == 5 && requested_count >= 8) { // HOST_PRIORITY_INFO
            const std::array<std::uint32_t, 18> reply {
                18,
                72,
                *local_port,
                0,
                0,
                *message_id + 100,
                0x00000000U,
                0x00000001U,
                0, // KERN_SUCCESS
                8, // host_info_outCnt
                80, // MINPRI_KERNEL
                80, // MINPRI_KERNEL
                64, // MINPRI_RESERVED
                31, // BASEPRI_DEFAULT
                0, // DEPRESSPRI
                0, // IDLEPRI
                0, // MINPRI_USER
                79, // MAXPRI_RESERVED
            };
            for (std::size_t index = 0; index < reply.size(); ++index) {
                if (!memory_.write32(message_address +
                                         static_cast<std::uint32_t>(index * 4U),
                        reply[index])) {
                    registers[0] = 0x10004008U;
                    return true;
                }
            }
            registers[0] = 0;
            return true;
        }
    }
    if (*message_id ==
            mig_message_id(xnu792::mig::mach_host::Routine::host_page_size) &&
        registers[3] >= 40) {
        const std::array<std::uint32_t, 10> reply {
            18, // MOVE_SEND_ONCE reply right
            40, // message size
            *local_port, // reply destination
            0,
            0,
            *message_id + 100, // MIG reply ID
            0x00000000U, // NDR bytes 0..3
            0x00000001U, // little-endian NDR, bytes 4..7
            0, // KERN_SUCCESS
            AddressSpace::page_size,
        };
        for (std::size_t index = 0; index < reply.size(); ++index) {
            if (!memory_.write32(
                    message_address + static_cast<std::uint32_t>(index * 4U),
                    reply[index])) {
                registers[0] = 0x10004008U; // MACH_RCV_INVALID_DATA
                return true;
            }
        }
        registers[0] = 0; // MACH_MSG_SUCCESS
        return true;
    }
    return false;
}

} // namespace ilemu
