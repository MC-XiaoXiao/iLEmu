#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../support.hpp"
#include "wire_reply.hpp"

namespace ilemu {
namespace {

    using namespace mach_support;
    using namespace mach_vm_support;

    // The iPhoneOS ARM32 MIG stub publishes pointer-sized fields for this
    // mach_vm call. Its complex request is one task-port descriptor followed by
    // seven 32-bit words; the reply is the ordinary NDR/result prefix plus the
    // selected address and two protection words.
    constexpr std::uint32_t mach_vm_remap_identifier = 4813U;
    constexpr std::uint32_t descriptor_count = 1U;
    constexpr std::uint32_t request_size = 76U;
    constexpr std::uint32_t reply_size = 48U;
    constexpr std::uint32_t kern_no_space = 3U;
    constexpr std::uint32_t kern_invalid_argument = 4U;
    constexpr std::uint32_t vm_protection_read = 1U;
    constexpr std::uint32_t vm_protection_write = 2U;
    constexpr std::uint32_t vm_protection_execute = 4U;
    constexpr std::uint32_t maximum_inheritance = 2U;

    [[nodiscard]] std::optional<std::uint32_t> round_page_size(
        std::uint32_t size)
    {
        constexpr auto mask = AddressSpace::page_size - 1U;
        if (size == 0U ||
            size > std::numeric_limits<std::uint32_t>::max() - mask) {
            return std::nullopt;
        }
        return (size + mask) & ~mask;
    }

    [[nodiscard]] std::uint32_t darwin_permissions(MemoryPermission permissions)
    {
        std::uint32_t result = 0U;
        if (has_permission(permissions, MemoryPermission::Read))
            result |= vm_protection_read;
        if (has_permission(permissions, MemoryPermission::Write))
            result |= vm_protection_write;
        if (has_permission(permissions, MemoryPermission::Execute))
            result |= vm_protection_execute;
        return result;
    }

    [[nodiscard]] std::optional<std::uint32_t> find_remap_region(
        const AddressSpace& memory, std::uint32_t start, std::uint32_t size,
        std::uint32_t mask)
    {
        auto candidate = find_free_guest_region(memory, start, size);
        while (candidate && (*candidate & mask) != 0U) {
            if (*candidate > std::numeric_limits<std::uint32_t>::max() -
                                 AddressSpace::page_size) {
                return std::nullopt;
            }
            candidate = find_free_guest_region(
                memory, *candidate + AddressSpace::page_size, size);
        }
        return candidate;
    }

} // namespace

bool CompatibilityKernel::dispatch_mach_vm_remap_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    if (request.identifier != mach_vm_remap_identifier)
        return false;

    auto& registers = cpu.registers();
    const auto fail_transport = [&] {
        registers[0] = mach_receive_invalid_data;
        return true;
    };
    if (registers[2] < request_size || registers[3] < reply_size ||
        (request.bits & darwin::mig_wire::message_complex_bit) == 0U) {
        return fail_transport();
    }

    const auto body_count = memory_.read32(
        request.address + darwin::mig_wire::complex_descriptor_count_offset);
    const auto source_task_name = memory_.read32(
        request.address + darwin::mig_wire::descriptor_name_offset(0));
    const auto descriptor_metadata = memory_.read32(
        request.address + darwin::mig_wire::descriptor_metadata_offset(0));
    auto target_address = memory_.read32(
        request.address + darwin::mig_wire::complex_request_word(1, 0));
    const auto requested_size = memory_.read32(
        request.address + darwin::mig_wire::complex_request_word(1, 1));
    const auto mask = memory_.read32(
        request.address + darwin::mig_wire::complex_request_word(1, 2));
    const auto flags = memory_.read32(
        request.address + darwin::mig_wire::complex_request_word(1, 3));
    const auto source_address = memory_.read32(
        request.address + darwin::mig_wire::complex_request_word(1, 4));
    const auto copy = memory_.read32(
        request.address + darwin::mig_wire::complex_request_word(1, 5));
    const auto inheritance = memory_.read32(
        request.address + darwin::mig_wire::complex_request_word(1, 6));
    if (!body_count || !source_task_name || !descriptor_metadata ||
        !target_address || !requested_size || !mask || !flags ||
        !source_address || !copy || !inheritance) {
        return fail_transport();
    }

    const auto requested_target_address = *target_address;
    const auto size = round_page_size(*requested_size);
    const auto descriptor_type =
        *descriptor_metadata >> darwin::mig_wire::descriptor_type_shift;
    const auto descriptor_disposition =
        (*descriptor_metadata >>
            darwin::mig_wire::descriptor_disposition_shift) &
        0xffU;
    std::optional<std::uint32_t> target_pid;
    std::optional<std::uint32_t> source_pid;
    {
        std::lock_guard lock { shared_state_->mach_mutex };
        target_pid = target_task_for_port(
            *shared_state_, process_.pid, request.remote_port);
        source_pid = target_task_for_port(
            *shared_state_, process_.pid, *source_task_name);
    }

    std::uint32_t result = kern_success;
    if (*body_count != descriptor_count || descriptor_type != 0U ||
        descriptor_disposition != darwin::mig_wire::disposition_copy_send ||
        !size || *source_address % AddressSpace::page_size != 0U ||
        *inheritance > maximum_inheritance || !target_pid || !source_pid ||
        *target_pid != process_.pid) {
        result = kern_invalid_argument;
    }

    std::optional<SharedTaskMemoryRange> source;
    if (result == kern_success && *source_pid == process_.pid) {
        const auto region = memory_.mapping_region_at_or_after(*source_address);
        const auto end = static_cast<std::uint64_t>(*source_address) + *size;
        if (region && region->address <= *source_address &&
            region->end >= end) {
            if (auto pages = memory_.share_pages(*source_address, *size)) {
                source = SharedTaskMemoryRange { std::move(*pages),
                    region->permissions };
            }
        }
    } else if (result == kern_success && task_memory_share_query_) {
        source = task_memory_share_query_(*source_pid, *source_address, *size);
    }
    if (result == kern_success && !source)
        result = kern_invalid_address;

    if (result == kern_success &&
        (*flags & darwin::mach::vm_flags_anywhere) != 0U) {
        *target_address =
            find_remap_region(memory_, default_dynamic_base, *size, *mask)
                .value_or(0U);
    }
    if (result == kern_success &&
        (*target_address == 0U ||
            *target_address % AddressSpace::page_size != 0U ||
            (*target_address & *mask) != 0U)) {
        result = kern_no_space;
    }

    if (result == kern_success &&
        guest_region_overlaps(memory_, *target_address, *size)) {
        if ((*flags & darwin::mach::vm_flags_overwrite) == 0U ||
            !memory_.unmap(*target_address, *size)) {
            result = kern_no_space;
        }
    }

    bool mapped = false;
    if (result == kern_success) {
        auto pages = std::move(source->pages);
        if (*copy != 0U) {
            for (auto& page : pages)
                page = std::make_shared<GuestPageBacking>(*page);
        }
        mapped = memory_.map_page_backings(*target_address, *size,
            source->permissions, pages,
            *copy != 0U ? AddressSpace::PageMappingMode::CopyOnWrite
                        : AddressSpace::PageMappingMode::Shared);
        if (!mapped)
            result = kern_no_space;
    }

    const auto protection =
        source ? darwin_permissions(source->permissions) : 0U;
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
        *target_address,
        protection,
        protection,
    };
    if (!write_words(memory_, request.address, reply))
        return fail_transport();

    output_.write("[vm] remap pid=" + std::to_string(process_.pid) +
                  " target=" + std::to_string(target_pid.value_or(0U)) +
                  " requested=" + std::to_string(requested_target_address) +
                  " address=" + std::to_string(*target_address) +
                  " size=" + std::to_string(size.value_or(0U)) + " mask=" +
                  std::to_string(*mask) + " flags=" + std::to_string(*flags) +
                  " source-pid=" + std::to_string(source_pid.value_or(0U)) +
                  " source=" + std::to_string(*source_address) +
                  " copy=" + std::to_string(*copy != 0U) +
                  " inheritance=" + std::to_string(*inheritance) +
                  " protection=" + std::to_string(protection) +
                  " result=" + std::to_string(result) + "\n");
    registers[0] = kern_success;
    return true;
}

} // namespace ilemu
