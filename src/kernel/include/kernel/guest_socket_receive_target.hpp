// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <kernel/guest_read_buffer.hpp>
#include <network/socket_receive_target.hpp>

namespace ilemu {
// Keep scalar and vector socket copyout on the same protection-checked path.
// The scalar iovec lives on the stack; it needs no allocation or import.
class GuestSocketReceiveTarget final : public SocketReceiveTarget {
public:
    GuestSocketReceiveTarget(AddressSpace& memory, std::uint32_t address,
        std::uint32_t size, std::span<const GuestReadVector> vectors = {})
        : memory_(memory), scalar_ {address, size},
          buffer_(address, vectors.empty()
              ? std::span<const GuestReadVector> {&scalar_, 1} : vectors) {}
    GuestSocketReceiveTarget(const GuestSocketReceiveTarget&) = delete;
    GuestSocketReceiveTarget& operator=(const GuestSocketReceiveTarget&) = delete;
    bool copy(std::span<const std::byte> bytes) override
    {
        failed_ = !buffer_.copy(memory_, bytes);
        return !failed_;
    }
    [[nodiscard]] bool failed() const { return failed_; }
private:
    AddressSpace& memory_;
    GuestReadVector scalar_;
    GuestReadBuffer buffer_;
    bool failed_ {};
};
} // namespace ilemu
