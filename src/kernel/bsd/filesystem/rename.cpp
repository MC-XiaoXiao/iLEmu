// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "../support.hpp"
#include "filesystem/host_file_rename.hpp"
#include "kernel/kernel.hpp"
#include "foundation/rootfs_path_resolver.hpp"
#include "filesystem/hfs_metadata.hpp"
#include <algorithm>
#include <mutex>
#include <utility>

namespace ilemu {
void CompatibilityKernel::service_completed_file_renames()
{
    if (shared_state_->filesystem_renames_pending.load(
            std::memory_order_acquire) == 0 &&
        file_rename_effects_.empty())
        return;
    std::vector<std::shared_ptr<PendingFileRename>> completed;
    {
        std::unique_lock lock { shared_state_->filesystem_mutex,
            std::try_to_lock };
        if (!lock.owns_lock())
            return;
        std::lock_guard completions_lock {
            shared_state_->file_rename_completions_mutex
        };
        auto& completions = shared_state_->file_rename_completions;
        for (auto it = completions.begin(); it != completions.end();) {
            const auto result = (*it)->request->result();
            if (!result) {
                ++it;
                continue;
            }
            if (result->renamed) {
                if (result->replaced_identity) {
                    shared_state_->hfs_metadata_overrides.erase(
                        *result->replaced_identity);
                    shared_state_->hfs_named_attribute_overrides.erase(
                        *result->replaced_identity);
                }
            }
            completed.push_back(*it);
            it = completions.erase(it);
        }
    }
    // Poll the namespace lock without blocking the emulation thread.
    // Publish even if the initiating process no longer has a waiting thread.
    for (const auto& operation : completed) {
        const auto result = operation->request->result();
        if (result->renamed) {
            if (result->directory) {
                shared_state_->guest_file_generation_registry
                    ->publish_subtree_rename(
                        operation->source, operation->destination);
            } else {
                shared_state_->guest_file_generation_registry->publish_rename(
                    operation->source, operation->destination);
            }
        }
        operation->published.store(true, std::memory_order_release);
        shared_state_->filesystem_renames_pending.fetch_sub(
            1, std::memory_order_release);
    }
    for (auto it = file_rename_effects_.begin();
        it != file_rename_effects_.end();) {
        const auto& operation = **it;
        const auto result = operation.request->result();
        if (!result || !operation.published.load(std::memory_order_acquire)) {
            ++it;
            continue;
        }
        if (result->renamed)
            directory_entries_cache_.clear();
        if (result->renamed && !result->error) {
            for (auto& [descriptor, path] : file_descriptors_) {
                static_cast<void>(descriptor);
                const auto normalized = path.lexically_normal();
                if (normalized == operation.source.lexically_normal()) {
                    path = operation.destination;
                    continue;
                }
                const auto relative =
                    normalized.lexically_relative(operation.source);
                if (!relative.empty() && relative != "." &&
                    *relative.begin() != "..")
                    path = operation.destination / relative;
            }
        }
        it = file_rename_effects_.erase(it);
    }
}

bool CompatibilityKernel::filesystem_dispatch_conflicts_with_rename(
    Cpu& cpu, std::uint32_t number) const
{
    bool follow_final_symlink;
    switch (number) {
    case 6: // close releases an existing open description, not a pathname.
        // A legacy descriptor without an open description may still resolve
        // its path while releasing advisory locks; keep that fallback ordered.
        return file_descriptors_.contains(cpu.registers()[0]) &&
               !regular_file_open_descriptions_.contains(cpu.registers()[0]);
    case 188: // stat
    case 338: // stat64
    case 341: // stat64_extended
        follow_final_symlink = true;
        break;
    case 190: // lstat
    case 340: // lstat64
    case 342: // lstat64_extended
        follow_final_symlink = false;
        break;
    default:
        return true;
    }
    const auto guest_path = memory_.read_c_string(cpu.registers()[0]);
    if (!guest_path)
        return false; // Let the normal dispatcher report EFAULT.
    std::vector<std::filesystem::path> traversed;
    const auto resolved = RootfsPathResolver { rootfs_ }.resolve(
        *guest_path, guest_working_directory_, follow_final_symlink, &traversed);
    const auto within = [](const auto& path, const auto& subtree) {
        const auto relative = path.lexically_normal().lexically_relative(
            subtree.lexically_normal());
        return !relative.empty() && *relative.begin() != "..";
    };
    std::lock_guard lock { shared_state_->file_rename_completions_mutex };
    for (const auto& operation : shared_state_->file_rename_completions) {
        // Include sidecars and every traversed symlink, not just the resolved
        // target. A parent directory counts its direct entries, not its subtree.
        for (const auto& affected : { operation->source, operation->destination,
                 hfs::MetadataProvider::resource_sidecar(operation->source),
                 hfs::MetadataProvider::resource_sidecar(operation->destination) }) {
            if (within(resolved, affected) ||
                affected.parent_path().lexically_normal() ==
                    resolved.lexically_normal() ||
                std::any_of(traversed.begin(), traversed.end(),
                    [&](const auto& path) { return within(path, affected); }))
                return true;
        }
    }
    return false;
}

bool CompatibilityKernel::defer_filesystem_dispatch(
    Cpu& cpu, std::uint32_t number)
{
    pending_filesystem_dispatches_.insert_or_assign(cpu.processor_id(),
        PendingFilesystemDispatch { number, cpu.registers() });
    process_.waiting_for_events = true;
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
    return true;
}

bool CompatibilityKernel::deliver_pending_filesystem_dispatch(Cpu& cpu)
{
    service_completed_file_renames();
    if (shared_state_->filesystem_renames_pending.load(
            std::memory_order_acquire) != 0)
        return false;
    const auto found = pending_filesystem_dispatches_.find(cpu.processor_id());
    if (found == pending_filesystem_dispatches_.end())
        return false;
    const auto pending = found->second;
    pending_filesystem_dispatches_.erase(found);
    cpu.registers() = pending.registers;
    cpu.clear_halt();
    process_.waiting_for_events = false;
    dispatch_bsd(cpu, pending.number);
    return !has_pending_event_locked(cpu.processor_id());
}

void CompatibilityKernel::dispatch_bsd_filesystem_rename(Cpu& cpu)
{
    const auto source_path = memory_.read_c_string(cpu.registers()[0]);
    const auto destination_path = memory_.read_c_string(cpu.registers()[1]);
    if (!source_path || !destination_path) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    auto source = resolve_guest_path(*source_path, false);
    auto destination = resolve_guest_path(*destination_path, false);
    if (source.lexically_normal() == rootfs_.lexically_normal()) {
        bsd_error(cpu, 16U); // EBUSY
        return;
    }
    std::shared_ptr<HostFileRenamer> renamer;
    {
        std::unique_lock lock { shared_state_->filesystem_mutex,
            std::try_to_lock };
        if (!lock.owns_lock() || shared_state_->filesystem_renames_pending.load(
                                     std::memory_order_acquire) != 0) {
            static_cast<void>(defer_filesystem_dispatch(cpu, 128));
            return;
        }
        if (!shared_state_->file_renamer)
            shared_state_->file_renamer = std::make_shared<HostFileRenamer>();
        renamer = shared_state_->file_renamer;
        shared_state_->filesystem_renames_pending.fetch_add(
            1, std::memory_order_release);
    }
    auto request = renamer->rename(
        rootfs_, source, destination);
    auto operation = std::make_shared<PendingFileRename>();
    operation->source = std::move(source);
    operation->destination = std::move(destination);
    operation->request = std::move(request);
    {
        std::lock_guard lock { shared_state_->file_rename_completions_mutex };
        shared_state_->file_rename_completions.push_back(operation);
    }
    file_rename_effects_.push_back(operation);
    pending_file_renames_.insert_or_assign(
        cpu.processor_id(), std::move(operation));
    process_.waiting_for_events = true;
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}

bool CompatibilityKernel::deliver_pending_file_rename(Cpu& cpu)
{
    service_completed_file_renames();
    const auto pending = pending_file_renames_.find(cpu.processor_id());
    if (pending == pending_file_renames_.end())
        return false;
    const auto result = pending->second->request->result();
    if (!result || !pending->second->published.load(std::memory_order_acquire))
        return false;
    if (result->error)
        bsd_error(cpu, bsd_support::darwin_filesystem_error(result->error));
    else
        bsd_success(cpu, 0);
    pending_file_renames_.erase(pending);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
}
} // namespace ilemu
