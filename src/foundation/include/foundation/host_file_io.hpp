// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "foundation/host_resource_controller.hpp"
#include <span>

namespace ilemu {

// Host-owned input and output only. The kernel retains and commits the guest
// cursor, copyout, mapping visibility and wakeup separately.
class HostFileIoRequest {
public:
    ~HostFileIoRequest();
    [[nodiscard]] bool ready() const
    {
        return ready_.load(std::memory_order_acquire);
    }
    [[nodiscard]] int error() const { return error_; }
    [[nodiscard]] std::uint64_t offset() const { return offset_; }
    [[nodiscard]] std::span<const std::byte> bytes() const { return bytes_; }
    // Called only after ready(); the worker has released ownership of bytes.
    [[nodiscard]] std::vector<std::byte> take_bytes()
    {
        return std::move(bytes_);
    }

private:
    friend class HostFileIo;
    HostFileIoRequest(
        int descriptor, std::uint64_t offset, std::vector<std::byte> bytes);
    void execute(bool write, bool append);
    int descriptor_ { -1 };
    std::uint64_t offset_;
    std::vector<std::byte> bytes_;
    int error_ { };
    std::atomic<bool> ready_ { false };
};

// The kernel's bounded FIFO admits one host request at a time, retaining its
// slot until kernel commit. Reuse mandatory maintenance work admission.
class HostFileIo {
public:
    ~HostFileIo();
    [[nodiscard]] std::shared_ptr<HostFileIoRequest> submit(int descriptor,
        std::uint64_t offset, std::vector<std::byte> bytes, bool write,
        bool append);

private:
    HostResourceController worker_;
};
} // namespace ilemu
