// This Source Code Form is subject to the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "../support.hpp"
#include "kernel/kernel.hpp"
#include <system_error>

namespace ilemu {
std::shared_ptr<FileIoOperation> CompatibilityKernel::prepare_file_io(Cpu& cpu,
    std::uint32_t fd, FileIoOperation::Kind kind, std::uint32_t address,
    std::size_t size, std::optional<std::uint64_t> position,
    std::vector<GuestReadVector> vectors, bool asynchronous)
{
    const TaskVmEvents::Scope user_access { memory_.task_vm_events() };
    if (size > bsd_support::maximum_io ||
        (position && *position > static_cast<std::uint64_t>(INT64_MAX))) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return { };
    }
    const auto file = file_descriptors_.find(fd);
    if (file == file_descriptors_.end()) {
        bsd_error(cpu, darwin::error::bad_file_descriptor);
        return { };
    }
    const auto flags =
        file_status_flags_.contains(fd)
            ? static_cast<std::uint32_t>(file_status_flags_.at(fd))
            : darwin::open_flag::read_only;
    const auto access = flags & darwin::open_flag::access_mode;
    if ((kind == FileIoOperation::Kind::Read &&
            access == darwin::open_flag::write_only) ||
        ((kind == FileIoOperation::Kind::Write ||
             (asynchronous && kind == FileIoOperation::Kind::Synchronize)) &&
            access == darwin::open_flag::read_only)) {
        bsd_error(cpu, darwin::error::bad_file_descriptor);
        return { };
    }
    if (asynchronous && !shared_state_->file_io.can_submit(size)) {
        bsd_error(cpu, darwin::error::would_block);
        return { };
    }
    const auto description = ensure_regular_file_open_description(fd);
    if (!description) {
        bsd_error(cpu, darwin::error::bad_file_descriptor);
        return { };
    }
    auto operation = std::make_shared<FileIoOperation>();
    operation->kind = kind;
    operation->synchronous = !asynchronous;
    operation->owner = process_.pid;
    operation->descriptor = fd;
    operation->description = description;
    operation->description_identifier = description->identifier();
    operation->path = file->second;
    operation->cursor = file_offsets_[fd];
    if (kind == FileIoOperation::Kind::Write)
        operation->mapping_identity =
            shared_state_->shared_mapping_page_cache->retain_file_object(
                description->host_descriptor());
    operation->position = position;
    operation->append = kind == FileIoOperation::Kind::Write &&
                        (flags & darwin::open_flag::append) != 0 &&
                        (!position || asynchronous);
    // Native pwrite ignores O_APPEND. aio_write preserves append semantics,
    // including advancing the fileglob cursor (kern_aio.c:do_aio_write).
    if (operation->append && asynchronous)
        operation->position.reset();
    operation->address = address;
    operation->vectors = std::move(vectors);
    operation->size = size;
    if (shared_state_->file_io.can_submit(size)) {
        if (!copy_file_io_input(*operation) && !asynchronous) {
            bsd_error(cpu, operation->error);
            return { };
        }
        shared_state_->file_io.push(operation);
    } else {
        // Synchronous backpressure retains only the description, cursor and
        // user arguments. Copyin occurs after reserving payload capacity.
        shared_state_->file_io.wait(operation);
    }
    return operation;
}

bool CompatibilityKernel::copy_file_io_input(FileIoOperation& operation)
{
    const TaskVmEvents::Scope user_access { memory_.task_vm_events() };
    if (operation.kind == FileIoOperation::Kind::Write &&
        !operation.vectors.empty()) {
        operation.input.reserve(operation.size);
        for (const auto& vector : operation.vectors) {
            auto bytes = memory_.read_bytes(vector.address, vector.length);
            if (!bytes) {
                if (!operation.input.empty())
                    return true; // Preserve a completed writev prefix.
                operation.error = darwin::error::bad_address;
                return false;
            }
            operation.input.insert(
                operation.input.end(), bytes->begin(), bytes->end());
        }
    } else if (operation.kind == FileIoOperation::Kind::Write) {
        auto bytes = memory_.read_bytes(operation.address, operation.size);
        if (!bytes) {
            operation.error = darwin::error::bad_address;
            return false;
        }
        operation.input = std::move(*bytes);
    } else if (operation.kind == FileIoOperation::Kind::Read) {
        operation.input.resize(operation.size);
    }
    return true;
}

void CompatibilityKernel::wait_for_file_io(
    Cpu& cpu, std::shared_ptr<FileIoOperation> operation)
{
    pending_file_ios_.insert_or_assign(
        cpu.processor_id(), std::move(operation));
    process_.waiting_for_events = true;
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}

void CompatibilityKernel::service_file_io()
{
    auto& queue = shared_state_->file_io;
    for (;;) {
        if (const auto waiting = queue.admission();
            waiting &&
            (waiting->abandoned || (waiting->owner == process_.pid &&
                                       queue.admits(waiting->size)))) {
            queue.retire_admission();
            if (waiting->abandoned || !copy_file_io_input(*waiting)) {
                waiting->committed = true;
                waiting->description.reset();
                shared_state_->note_io_event_transition();
                continue;
            }
            queue.push(waiting);
        }
        const auto operation = queue.front();
        if (!operation)
            break;
        if (operation->committed) {
            if (operation->abandoned) {
                queue.pop();
                continue;
            }
            break; // Keep a synchronous completion ordered through wakeup.
        }
        if (!operation->ready() ||
            (!operation->abandoned && operation->owner != process_.pid))
            break;
        const auto host_error = operation->cancelled || operation->error ? 0
                                : operation->sync_request
                                    ? *operation->sync_request->result()
                                    : operation->request->error();
        if (operation->cancelled) {
            operation->error = darwin::error::operation_canceled;
        } else if (operation->error) {
            // AIO copyin faults are completion errors after accepted
            // submission.
        } else if (host_error) {
            operation->error = bsd_support::darwin_filesystem_error(
                std::error_code { host_error, std::generic_category() });
        } else if (operation->kind == FileIoOperation::Kind::Synchronize) {
            operation->result = 0;
        } else {
            if (operation->kind == FileIoOperation::Kind::Write &&
                operation->mapping_identity &&
                operation->mapping_identity->writeback_revision.load(
                    std::memory_order_acquire) !=
                    operation->writeback_revision) {
                // VM writeback may have overwritten the completed host bytes
                // while the guest kept running. Reapply the same transferred
                // range before publishing mappings/cursor/wakeup. Append keeps
                // its already-selected offset and never appends a second time.
                queue.repeat_write(*operation);
                break;
            }
            const auto& request = operation->request;
            const auto bytes = request->bytes();
            std::size_t transferred = bytes.size();
            if (operation->kind == FileIoOperation::Kind::Write) {
                const auto descriptor =
                    operation->description->host_descriptor();
                static_cast<void>(shared_state_->shared_mapping_page_cache
                        ->reflect_descriptor_write(
                            descriptor, request->offset(), bytes));
                static_cast<void>(shared_state_->guest_file_generation_registry
                        ->publish_descriptor(operation->path, descriptor,
                            GuestFileMutationKind::Write));
            } else if (!operation->abandoned) {
                const GuestReadBuffer target { operation->address,
                    operation->vectors };
                if (!target.copy(memory_, bytes, &transferred))
                    operation->error = darwin::error::bad_address;
            }
            if (!operation->position) {
                // Scalar read historically advances by the host transfer even
                // on copyout failure; readv advances only the copied prefix.
                const auto advance =
                    operation->vectors.empty() ? bytes.size() : transferred;
                operation->cursor = request->offset() + advance;
            }
            if (!operation->error)
                operation->result = static_cast<std::int32_t>(bytes.size());
        }
        operation->request.reset();
        operation->sync_request.reset();
        operation->description.reset();
        operation->mapping_identity.reset();
        operation->committed = true;
        if (!operation->synchronous || operation->abandoned)
            queue.pop();
        shared_state_->note_io_event_transition();
        if (!operation->abandoned && operation->notification_signal)
            static_cast<void>(deliver_signal(operation->notification_signal));
        if (operation->synchronous && !operation->abandoned)
            break;
    }
}

bool CompatibilityKernel::deliver_pending_file_io(Cpu& cpu)
{
    service_file_io();
    const auto found = pending_file_ios_.find(cpu.processor_id());
    if (found == pending_file_ios_.end() || !found->second->committed)
        return false;
    const auto operation = found->second;
    pending_file_ios_.erase(found);
    if (shared_state_->file_io.front() == operation)
        shared_state_->file_io.pop();
    if (operation->error)
        bsd_error(cpu, operation->error);
    else
        bsd_success(cpu, static_cast<std::uint32_t>(operation->result));
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
}

void CompatibilityKernel::abandon_file_io()
{
    // The queue retains host effects and descriptions after exec/exit. A new
    // address space or recycled processor slot must never receive old copyout.
    shared_state_->file_io.abandon(process_.pid);
    pending_file_ios_.clear();
    pending_aio_suspends_.clear();
    aio_completions_.clear();
}
} // namespace ilemu
