// This Source Code Form is subject to the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "filesystem/bsd_file_lock.hpp"
#include "foundation/host_file_io.hpp"
#include "foundation/host_file_sync.hpp"
#include "kernel/guest_read_buffer.hpp"
#include "kernel/open_file_offset.hpp"
#include <deque>
#include <filesystem>

namespace ilemu {
struct FileIoOperation {
    enum class Kind { Read, Write, Synchronize };
    Kind kind;
    std::uint32_t owner;
    std::uint32_t descriptor;
    std::uint64_t description_identifier { };
    std::shared_ptr<bsd::RegularFileOpenDescription> description;
    std::filesystem::path path;
    OpenFileOffset cursor;
    std::shared_ptr<const GuestFileIoState> mapping_identity;
    std::uint64_t writeback_revision { };
    std::optional<std::uint64_t> position;
    bool append { false };
    std::uint32_t address { };
    std::vector<GuestReadVector> vectors;
    std::vector<std::byte> input;
    std::size_t size { };
    std::shared_ptr<HostFileIoRequest> request;
    std::shared_ptr<HostFileSyncRequest> sync_request;
    std::uint32_t notification_signal { };
    std::int32_t result { -1 };
    std::uint32_t error { };
    bool synchronous { true };
    bool abandoned { false };
    bool cancelled { false };
    bool committed { false };
    [[nodiscard]] bool ready() const;
};

// Accessed only during serialized kernel dispatch/completion. Slots include
// host execution AND kernel commit, bounding all in-flight payload storage.
// FIFO commit also orders append EOF selection and shared dup/fork cursors.
class FileIoQueue {
public:
    [[nodiscard]] bool admits(std::size_t size) const;
    void push(std::shared_ptr<FileIoOperation> operation);
    [[nodiscard]] std::shared_ptr<FileIoOperation> front() const;
    [[nodiscard]] bool empty() const
    {
        return operations_.empty() && admissions_.empty();
    }
    [[nodiscard]] bool can_submit(std::size_t size) const
    {
        return admissions_.empty() && admits(size);
    }
    void wait(std::shared_ptr<FileIoOperation> operation)
    {
        admissions_.push_back(std::move(operation));
    }
    [[nodiscard]] std::shared_ptr<FileIoOperation> admission() const;
    void retire_admission() { admissions_.pop_front(); }
    void pop();
    void abandon(std::uint32_t owner);
    void repeat_write(FileIoOperation& operation);
    void cancel(const std::shared_ptr<FileIoOperation>& operation);
    [[nodiscard]] bool owns(std::uint32_t owner) const;

private:
    void start();
    static constexpr std::size_t maximum_operations = 64;
    static constexpr std::size_t maximum_bytes = 64U * 1024U * 1024U;
    std::size_t bytes_ { };
    std::deque<std::shared_ptr<FileIoOperation>> operations_;
    std::deque<std::shared_ptr<FileIoOperation>> admissions_;
    std::unique_ptr<HostFileIo> io_;
    std::unique_ptr<HostFileSynchronizer> synchronizer_;
};
} // namespace ilemu
