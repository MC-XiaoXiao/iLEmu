#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/vm_map_mig_ids.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include "../support.hpp"
#include "wire_reply.hpp"

namespace ilemu {
namespace {

    using namespace mach_support;
    using namespace mach_vm_support;

    constexpr std::uint32_t vm_map_request_size = 84U;
    constexpr std::uint32_t vm_map_64_request_size = 88U;
    // Darwin 8 also publishes the same ARM32 wire contract through the
    // mach_vm subsystem.  Its mach_vm_map routine is numbered relative to the
    // 4800 subsystem base instead of vm_map's 3800 base.
    constexpr std::uint32_t mach_vm_map_identifier = 4811U;
    constexpr std::uint32_t reply_size = 40U;
    constexpr std::uint32_t kern_protection_failure = 2U;
    constexpr std::uint32_t kern_no_space = 3U;
    constexpr std::uint32_t kern_invalid_argument = 4U;
    constexpr std::uint32_t vm_protection_mask = 0x7U;

    [[nodiscard]] MemoryPermission memory_permissions(std::uint32_t protection)
    {
        MemoryPermission result = MemoryPermission::None;
        if ((protection & 1U) != 0)
            result |= MemoryPermission::Read;
        if ((protection & 2U) != 0)
            result |= MemoryPermission::Write;
        if ((protection & 4U) != 0)
            result |= MemoryPermission::Execute;
        return result;
    }

    [[nodiscard]] std::optional<std::uint32_t> round_page_size(
        std::uint32_t size)
    {
        constexpr auto mask = AddressSpace::page_size - 1U;
        if (size == 0 ||
            size > std::numeric_limits<std::uint32_t>::max() - mask)
            return std::nullopt;
        return (size + mask) & ~mask;
    }

} // namespace

bool CompatibilityKernel::dispatch_mach_vm_map_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    using xnu792::mig::vm_map::Routine;
    const auto is_mach_vm = request.identifier == mach_vm_map_identifier;
    const auto is_64 =
        request.identifier == mig_message_id(Routine::vm_map_64) || is_mach_vm;
    if (!is_64 && request.identifier != mig_message_id(Routine::vm_map))
        return false;

    auto& registers = cpu.registers();
    const auto required_request_size =
        is_64 ? vm_map_64_request_size : vm_map_request_size;
    if (registers[2] < required_request_size || registers[3] < reply_size) {
        registers[0] = mach_receive_invalid_data;
        return true;
    }

    const auto& arguments = is_64 ? xnu792::mig::vm_map::vm_map_64_arguments
                                  : xnu792::mig::vm_map::vm_map_arguments;
    auto address =
        memory_.read32(request.address + arguments[1].request_offset);
    const auto requested_size =
        memory_.read32(request.address + arguments[2].request_offset);
    const auto alignment_mask =
        memory_.read32(request.address + arguments[3].request_offset);
    const auto flags =
        memory_.read32(request.address + arguments[4].request_offset);
    const auto object_name =
        memory_.read32(request.address + arguments[5].request_offset);
    std::optional<std::uint64_t> object_offset;
    if (is_64) {
        object_offset =
            memory_.read64(request.address + arguments[6].request_offset);
    } else if (const auto value = memory_.read32(
                   request.address + arguments[6].request_offset)) {
        object_offset = *value;
    }
    const auto copy =
        memory_.read32(request.address + arguments[7].request_offset);
    const auto protection =
        memory_.read32(request.address + arguments[8].request_offset);
    const auto maximum_protection =
        memory_.read32(request.address + arguments[9].request_offset);
    const auto inheritance =
        memory_.read32(request.address + arguments[10].request_offset);
    if (!address || !requested_size || !alignment_mask || !flags || !object_name ||
        !object_offset || !copy || !protection || !maximum_protection ||
        !inheritance) {
        registers[0] = mach_receive_invalid_data;
        return true;
    }

    const auto requested_address = *address;
    const auto size = round_page_size(*requested_size);
    std::uint32_t result = kern_success;
    if (!size ||
        ((*protection | *maximum_protection) & ~vm_protection_mask) != 0 ||
        (*protection & *maximum_protection) != *protection ||
        *inheritance > static_cast<std::uint32_t>(VmInheritance::None)) {
        result = kern_invalid_argument;
    }

    bool targets_current_task = false;
    std::optional<KernelSharedState::MachMemoryEntry> entry;
    if (result == kern_success) {
        std::lock_guard lock { shared_state_->mach_mutex };
        targets_current_task =
            target_task_for_port(*shared_state_, process_.pid,
                request.remote_port) == process_.pid;
        if (*object_name != 0) {
            const auto object = resolve_name_with_right(*shared_state_,
                process_.pid, *object_name, xnu792::ipc::Right::Send);
            const auto found =
                object ? shared_state_->mach_memory_entries.find(*object)
                       : shared_state_->mach_memory_entries.end();
            if (found != shared_state_->mach_memory_entries.end())
                entry = found->second;
        }
    }
    if (result == kern_success && !targets_current_task)
        result = kern_invalid_argument;
    if (result == kern_success && *object_name != 0 && !entry)
        result = kern_invalid_argument;

    if (result == kern_success &&
        (*flags & darwin::mach::vm_flags_anywhere) != 0) {
        *address = find_free_guest_region(
            memory_, default_dynamic_base, *size, *alignment_mask)
                       .value_or(0);
    }
    if (result == kern_success &&
        (*address == 0 || *address % AddressSpace::page_size != 0 ||
            (*address & *alignment_mask) != 0U ||
            guest_region_overlaps(memory_, *address, *size))) {
        result = kern_no_space;
    }

    bool map_ok = false;
    if (result == kern_success && entry) {
        if (*object_offset % AddressSpace::page_size != 0 ||
            *object_offset > entry->size ||
            *size > entry->size - *object_offset) {
            result = kern_invalid_argument;
        } else if ((*protection & entry->protection) != *protection ||
                   (*maximum_protection & entry->protection) !=
                       *maximum_protection) {
            result = kern_protection_failure;
        } else {
            const auto first_page =
                entry->first_page + *object_offset / AddressSpace::page_size;
            const auto page_count = *size / AddressSpace::page_size;
            if (!entry->object || first_page > entry->object->pages.size() ||
                page_count > entry->object->pages.size() - first_page) {
                result = kern_invalid_argument;
            } else {
                const std::span<const std::shared_ptr<GuestPageBacking>> pages {
                    entry->object->pages.data() + first_page, page_count
                };
                const auto mode =
                    *copy != 0 ? AddressSpace::PageMappingMode::CopyOnWrite
                               : AddressSpace::PageMappingMode::Shared;
                map_ok = memory_.map_page_backings(*address, *size,
                    memory_permissions(*protection), pages, mode);
            }
        }
    } else if (result == kern_success) {
        map_ok = memory_.map(*address, *size, memory_permissions(*protection));
    }
    if (result == kern_success && !map_ok)
        result = kern_no_space;
    if (result == kern_success &&
        !memory_.inherit(*address, *size,
            static_cast<VmInheritance>(*inheritance))) {
        static_cast<void>(memory_.unmap(*address, *size));
        result = kern_invalid_argument;
    }

    const std::array<std::uint32_t, reply_size / sizeof(std::uint32_t)> reply {
        darwin::mig_wire::message_bits(
            darwin::mig_wire::disposition_move_send_once),
        reply_size,
        request.local_port,
        0U,
        0U,
        request.identifier + 100U,
        0U,
        1U,
        result,
        *address,
    };
    if (!write_words(memory_, request.address, reply)) {
        registers[0] = mach_receive_invalid_data;
        return true;
    }

    output_.write(
        "[vm] map pid=" + std::to_string(process_.pid) + " interface=" +
        (is_mach_vm ? std::string { "mach_vm" } : std::string { "vm_map" }) +
        " requested=" + std::to_string(requested_address) + " address=" +
        std::to_string(*address) + " size=" + std::to_string(size.value_or(0)) +
        " mask=" + std::to_string(*alignment_mask) +
        " flags=" + std::to_string(*flags) +
        " object=" + std::to_string(*object_name) + " offset=" +
        std::to_string(*object_offset) + " copy=" + std::to_string(*copy != 0) +
        " protection=" + std::to_string(*protection) +
        " inheritance=" + std::to_string(*inheritance) +
        " result=" + std::to_string(result) + "\n");
    registers[0] = kern_success;
    return true;
}

} // namespace ilemu
