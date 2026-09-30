// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../support.hpp"
#include "protection.hpp"
#include "mapping_range.hpp"
#include "foundation/host_file_mapping.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_mmap(Cpu& cpu)
{
    PendingFileMapping mapping;
    std::copy_n(cpu.registers().begin(), mapping.arguments.size(),
        mapping.arguments.begin());
    complete_bsd_mapping(cpu, std::move(mapping));
}

bool CompatibilityKernel::deliver_pending_file_mapping(Cpu& cpu)
{
    const auto found = pending_file_mappings_.find(cpu.processor_id());
    if (found == pending_file_mappings_.end() ||
        !found->second.preparation->ready())
        return false;
    auto mapping = std::move(found->second);
    pending_file_mappings_.erase(found);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    complete_bsd_mapping(cpu, std::move(mapping));
    return true;
}

void CompatibilityKernel::complete_bsd_mapping(
    Cpu& cpu, PendingFileMapping mapping)
{
    auto address = mapping.arguments[0];
    const auto size = mapping.arguments[1];
    const auto protection = mapping.arguments[2];
    const auto flags = mapping.arguments[3];
    const auto fd = mapping.arguments[4];
    const auto offset =
        static_cast<std::uint64_t>(mapping.arguments[5]) |
        (static_cast<std::uint64_t>(mapping.arguments[6]) << 32U);
    const auto range = bsd_vm::MappingRange::from_request(address, size, offset,
        (flags & darwin::map_flag::fixed) != 0U);
    if (!range) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
    }
    address = range->address;
    const auto mapped_size = range->size;
    const bsd_vm::Protection contract { shared_state_->darwin_abi.abi_epoch };
    const auto permissions = contract.permissions(protection);
    const auto shared = (flags & darwin::map_flag::shared) != 0;
    mapping.attributes.inheritance =
        shared ? VmInheritance::Share : VmInheritance::Copy;
    std::shared_ptr<GuestFileBacking> backing;
    if ((flags & darwin::map_flag::anonymous) == 0) {
        if (!mapping.preparation) {
            const auto found = file_descriptors_.find(fd);
            if (found == file_descriptors_.end()) {
                bsd_error(cpu, bsd_support::bad_file_descriptor);
                return;
            }
            mapping.path = found->second;
            const auto descriptor_flags = file_status_flags_.contains(fd)
                ? file_status_flags_.at(fd) : darwin::open_flag::read_only;
            mapping.cache = shared ? shared_state_->shared_mapping_page_cache
                                   : memory_.file_page_cache();
            std::shared_ptr<HostFileMappingPreparer> preparer;
            bool posix_shared_memory;
            {
                const std::lock_guard lock { shared_state_->filesystem_mutex };
                posix_shared_memory =
                    shared_state_->volatile_shared_memory_backings.contains(
                        mapping.path);
                if (shared) {
                    mapping.mode = posix_shared_memory
                        ? AddressSpace::PageMappingMode::Shared
                        : AddressSpace::PageMappingMode::SharedFile;
                }
                preparer = shared_state_->file_mapping_preparer;
            }
            const auto access = contract.file_mapping(permissions, descriptor_flags,
                shared, posix_shared_memory
                    ? bsd_vm::Protection::Object::PosixSharedMemory
                    : bsd_vm::Protection::Object::File);
            if (access.error != 0U) {
                bsd_error(cpu, access.error);
                return;
            }
            // Retain the descriptor's limits across asynchronous preparation;
            // completion must not re-read a closed or reused descriptor.
            mapping.attributes = access.attributes;
            mapping.preparation =
                preparer ? preparer->prepare(
                               mapping.cache, mapping.path, range->file_offset,
                               mapped_size)
                         : FileMappingPreparation::begin(mapping.cache,
                               mapping.path, range->file_offset, mapped_size);
            if (!preparer && mapping.preparation)
                mapping.preparation->complete();
            if (!mapping.preparation) {
                bsd_error(cpu, shared ? bsd_support::invalid_argument
                                      : darwin::error::no_memory);
                return;
            }
        }
        const auto result = mapping.preparation->result();
        if (!result) {
            pending_file_mappings_.insert_or_assign(
                cpu.processor_id(), std::move(mapping));
            process_.waiting_for_events = true;
            bsd_success(cpu, 0);
            cpu.halt(Dynarmic::HaltReason::UserDefined5);
            return;
        }
        backing = *result;
        if (!backing) {
            bsd_error(cpu, (flags & darwin::map_flag::shared) != 0
                               ? bsd_support::invalid_argument
                               : darwin::error::no_memory);
            return;
        }
    }
    constexpr auto address_space_end = std::uint64_t { 1 } << 32U;
    const auto overlaps = [&](std::uint32_t candidate) {
        const auto end = static_cast<std::uint64_t>(candidate) + mapped_size;
        if (end > address_space_end) {
            return true;
        }
        const auto region = memory_.mapping_region_at_or_after(candidate);
        return region && static_cast<std::uint64_t>(region->address) < end;
    };
    if ((flags & darwin::map_flag::fixed) == 0) {
        if (address == 0 || overlaps(address)) {
            std::uint64_t candidate = 0x10000000U;
            bool found = false;
            while (candidate + mapped_size <= address_space_end) {
                const auto region = memory_.mapping_region_at_or_after(
                    static_cast<std::uint32_t>(candidate));
                const auto end = candidate + mapped_size;
                if (!region ||
                    static_cast<std::uint64_t>(region->address) >= end) {
                    address = static_cast<std::uint32_t>(candidate);
                    found = true;
                    break;
                }
                candidate =
                    (region->end + AddressSpace::page_size - 1U) &
                    ~(static_cast<std::uint64_t>(AddressSpace::page_size) - 1U);
            }
            if (!found) {
                bsd_error(cpu, darwin::error::no_memory);
                return;
            }
        }
    } else {
        if (static_cast<std::uint64_t>(address) + mapped_size >
            address_space_end) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        static_cast<void>(unmap_memory(cpu, address, mapped_size));
    }
    if ((flags & darwin::map_flag::anonymous) == 0) {
        if (!memory_.map_file_backing(address, mapped_size, permissions,
                std::move(backing), mapping.cache, mapping.mode,
                mapping.attributes)) {
            bsd_error(cpu, darwin::error::no_memory);
            return;
        }
        static_cast<void>(install_mapped_user_image(
            cpu, mapping.path, address, mapped_size, range->file_offset));
        if (mapping_trace_count_ < 64U) {
            output_.write("[mmap] pid=" + std::to_string(process_.pid) +
                          " address=" + std::to_string(address) +
                          " size=" + std::to_string(size) +
                          " offset=" + std::to_string(offset) +
                          " prot=" + std::to_string(protection) +
                          " flags=" + std::to_string(flags) +
                          " file=" + mapping.path.string() + "\n");
            ++mapping_trace_count_;
        }
    } else {
        if (!memory_.map(address, mapped_size, permissions, mapping.attributes)) {
            bsd_error(cpu, darwin::error::no_memory);
            return;
        }
        if (mapping_trace_count_ < 64U) {
            output_.write("[mmap] pid=" + std::to_string(process_.pid) +
                          " address=" + std::to_string(address) +
                          " size=" + std::to_string(size) +
                          " offset=" + std::to_string(offset) +
                          " prot=" + std::to_string(protection) +
                          " flags=" + std::to_string(flags) + " anonymous\n");
            ++mapping_trace_count_;
        }
    }
    bsd_success(cpu, address + range->page_offset);
    return;
}

} // namespace ilemu
