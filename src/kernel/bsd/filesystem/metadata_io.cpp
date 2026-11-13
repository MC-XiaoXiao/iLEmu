// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "../support.hpp"
#include "kernel/kernel.hpp"
#include "foundation/performance.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ilemu {
namespace {
    struct MetadataWork {
        std::filesystem::path path;
        std::optional<hfs::Metadata> metadata;
        hfs::MetadataOverride update;
        std::uint32_t error { darwin::error::io };
        bool created { };
        std::uint64_t started_ns { };
        std::uint64_t elapsed_ns { };
    };

    std::uint64_t steady_nanoseconds()
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<
            std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

void CompatibilityKernel::dispatch_bsd_filesystem_metadata(Cpu& cpu, std::uint32_t number)
{
    const auto& registers = cpu.registers();
    const auto path = number == 475U
        ? read_guest_path_at(cpu, registers[0], registers[1])
        : memory_.read_c_string(registers[0]);
    if (!path) {
        if (number != 475U)
            bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    auto work = std::make_shared<MetadataWork>();
    auto operation = std::make_shared<FileIoOperation>();
    operation->kind = FileIoOperation::Kind::HostCall;
    operation->owner = process_.pid;
    operation->descriptor = std::numeric_limits<std::uint32_t>::max();
    const auto root = rootfs_;
    const auto directory = working_directory(cpu.processor_id());
    const auto state = shared_state_;
    const auto provider = hfs_metadata_;
    const auto diagnostic = performance_counters().cpu_source_diagnostics_enabled();
    const auto report = [work, diagnostic, number, pid = process_.pid,
                            output = &output_] {
        if (diagnostic && work->elapsed_ns >= 50'000'000U) {
            std::ostringstream message;
            message << "[perf] filesystem-host pid=" << pid << " number=" << number
                    << " path=" << std::quoted(work->path.generic_string())
                    << " started-ns=" << work->started_ns
                    << " elapsed-ns=" << work->elapsed_ns;
            output->line(message.str());
        }
    };
    if (number == 220U) {
        const auto bitmap_count = memory_.read16(registers[1]);
        if (!bitmap_count || *bitmap_count != 5U) {
            bsd_error(cpu, bitmap_count ? bsd_support::invalid_argument : bsd_support::bad_address);
            return;
        }
        std::array<std::uint32_t, 5> masks { };
        for (std::uint32_t index = 0; index < masks.size(); ++index) {
            const auto mask = memory_.read32(registers[1] + 4U + index * 4U);
            if (!mask) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
            masks[index] = *mask;
        }
        const hfs::AttributeRequest request { masks[0], masks[1], masks[2], masks[3], masks[4] };
        if (!hfs::MetadataProvider::valid_request(request) ||
            (request.volume == 0U && (request.common & 0x00180000U) != 0U &&
                (registers[4] & 0x20U) == 0U) ||
            ((registers[4] & 8U) != 0U &&
                (request.common & hfs::attribute::common_returned_attributes) == 0U)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        const auto options = registers[4];
        const auto follow = (options & 1U) == 0U;
        const auto volume = hfs_volumes_.for_guest_path(*path);
        const auto mount_root = hfs_volumes_.is_mount_root(*path);
        operation->host_call = [work, root, directory, path = *path, provider,
                                  follow, request] {
            work->started_ns = steady_nanoseconds();
            work->path = bsd_support::resolve_guest_path(root, directory, path, follow);
            work->metadata = provider.query(work->path, follow,
                (request.directory & hfs::attribute::directory_entry_count) != 0U);
            work->error = work->metadata ? 0U : 2U;
            work->elapsed_ns = steady_nanoseconds() - work->started_ns;
        };
        operation->host_complete = [work, state, root, request, options, volume,
                                      mount_root, address = registers[2], capacity = registers[3], report](
                                      FileIoOperation& result, AddressSpace* memory) {
            report();
            result.error = work->error;
            if (result.error || !memory)
                return;
            auto metadata = *work->metadata;
            {
                const std::lock_guard lock { state->filesystem_mutex };
                const auto update = state->hfs_metadata_overrides.find(metadata.host_identity);
                if (update != state->hfs_metadata_overrides.end())
                    hfs::MetadataProvider::apply_override(metadata, update->second);
            }
            if (request.volume != 0U && !mount_root) {
                result.error = bsd_support::invalid_argument;
                return;
            }
            const auto guest = (std::filesystem::path { "/" } /
                work->path.lexically_relative(root)).lexically_normal().generic_string();
            auto bytes = request.volume != 0U
                ? hfs::MetadataProvider::pack_volume_attributes(metadata, volume, request)
                : (request.common & hfs::attribute::common_returned_attributes) != 0U
                    ? hfs::MetadataProvider::pack_returned_attributes(metadata, request, guest, (options & 8U) != 0U)
                    : hfs::MetadataProvider::pack_attributes(metadata, request, guest);
            const auto full_size = static_cast<std::uint32_t>(bytes.size());
            if (bytes.size() > capacity)
                bytes.resize(capacity);
            if (bytes.size() >= sizeof(std::uint32_t)) {
                const auto size = (options & 4U) != 0U ? full_size : static_cast<std::uint32_t>(bytes.size());
                for (std::size_t byte = 0; byte < sizeof(size); ++byte)
                    bytes[byte] = static_cast<std::byte>(size >> (byte * 8U));
            }
            const TaskVmEvents::Scope user_access { memory->task_vm_events() };
            if (!memory->copy_to_user(address, bytes))
                result.error = bsd_support::bad_address;
            else
                result.result = 0;
        };
    } else {
        const auto mode = registers[number == 475U ? 2U : 1U] & ~process_.file_creation_mask;
        operation->host_call = [work, root, directory, path = *path, provider, state,
                                  volumes = hfs_volumes_.snapshot(), mode, uid = process_.effective_uid,
                                  gid = process_.effective_gid]() mutable {
            work->started_ns = steady_nanoseconds();
            work->path = bsd_support::resolve_guest_path(root, directory, path, true);
            const auto execute = [&] {
                std::error_code error;
                if (std::filesystem::exists(work->path, error)) {
                    work->error = 17U;
                    return;
                }
                error.clear();
                if (!std::filesystem::is_directory(work->path.parent_path(), error)) {
                    work->error = 2U;
                    return;
                }
                const auto volume = volumes.for_host_path(work->path);
                const auto now = static_cast<std::uint32_t>(state->clock.now() / 1'000'000'000U);
                const auto quota_error = volumes.quotas.change(volume.mount_point,
                    uid, gid, 0, 1, uid == 0U, now);
                if (quota_error) {
                    work->error = quota_error;
                    return;
                }
                error.clear();
                if (!std::filesystem::create_directory(work->path, error)) {
                    static_cast<void>(volumes.quotas.change(volume.mount_point, uid, gid, 0, -1, true, now));
                    work->error = error == std::errc::permission_denied ? 13U : 5U;
                    return;
                }
                work->created = true;
                work->metadata = provider.query(work->path, true, false);
                if (!work->metadata)
                    return;
                auto& update = work->update;
                update.observe(*work->metadata);
                update.mode = 0040000U | (mode & 07777U);
                update.owner = uid;
                update.group = gid;
                const auto timestamp = bsd_support::guest_filesystem_timestamp(state->clock);
                update.creation_time = timestamp;
                update.modification_time = timestamp;
                update.change_time = timestamp;
                if ((volume.mount_flags & 1U) == 0U &&
                    hfs::MetadataProvider::persist_override(*work->metadata, update)) {
                    if (const auto current = provider.query(work->path, true, false)) {
                        const auto change = update.change_time;
                        update.observe(*current);
                        update.change_time = change;
                    }
                }
                work->error = 0;
            };
            execute();
            work->elapsed_ns = steady_nanoseconds() - work->started_ns;
        };
        operation->host_complete = [work, state, report](FileIoOperation& result, AddressSpace*) {
            report();
            if (work->metadata) {
                const std::lock_guard lock { state->filesystem_mutex };
                state->hfs_metadata_overrides[work->metadata->host_identity] = work->update;
            }
            if (work->created)
                state->guest_file_generation_registry->publish_subtree_create(work->path);
            result.error = work->error;
            result.result = 0;
        };
    }
    auto& queue = shared_state_->file_io;
    if (queue.can_submit(0))
        queue.push(operation);
    else
        queue.wait(operation);
    wait_for_file_io(cpu, std::move(operation));
}
} // namespace ilemu
