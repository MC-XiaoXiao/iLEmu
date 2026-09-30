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
          vectors_(vectors.empty()
              ? std::span<const GuestReadVector> {&scalar_, 1} : vectors),
          buffer_(address, vectors_) {}
    GuestSocketReceiveTarget(const GuestSocketReceiveTarget&) = delete;
    GuestSocketReceiveTarget& operator=(const GuestSocketReceiveTarget&) = delete;
    [[nodiscard]] bool can_copy_without_fault(std::size_t capacity) const override
    {
        // Bound speculative work for a short packet in a large user iovec.
        // This is an optimization budget, not a guest vector-count limit.
        constexpr std::size_t maximum_preflight_vectors = 8;
        if (vectors_.size() > maximum_preflight_vectors ||
            !memory_.owns_exclusive_access()) return false;
        for (const auto& vector : vectors_) {
            const auto count = std::min<std::size_t>(vector.length, capacity);
            if (count && !memory_.accessible(vector.address, count, MemoryPermission::Write))
                return false;
            capacity -= count;
            if (capacity == 0) return true;
        }
        return false;
    }
    bool copy(std::span<const std::byte> bytes) override
    {
        failed_ = !buffer_.copy(memory_, bytes);
        return !failed_;
    }
    // Local stream control boundaries may split one syscall into several
    // copy chunks. Keep ordinary copy()/host receive fast paths unchanged.
    bool copy_at(std::span<const std::byte> bytes, std::size_t offset)
    {
        if (offset == 0)
            return copy(bytes);
        const auto finish = [&](bool result) {
            failed_ = !result;
            return result;
        };
        if (bytes.empty())
            return finish(true);
        auto remaining = vectors_;
        while (!remaining.empty() && offset >= remaining.front().length) {
            offset -= remaining.front().length;
            remaining = remaining.subspan(1);
        }
        if (remaining.empty())
            return finish(false);
        const auto address = static_cast<std::uint64_t>(remaining.front().address) + offset;
        if (address >= (std::uint64_t {1} << 32U))
            return finish(false);
        const GuestReadVector first {static_cast<std::uint32_t>(address),
            remaining.front().length - static_cast<std::uint32_t>(offset)};
        const auto count = std::min<std::size_t>(bytes.size(), first.length);
        return finish(GuestReadBuffer {0, std::span<const GuestReadVector> {&first, 1}}.copy(
                   memory_, bytes.first(count)) &&
               (count == bytes.size() || (remaining.size() > 1 &&
                   GuestReadBuffer {0, remaining.subspan(1)}.copy(
                       memory_, bytes.subspan(count)))));
    }
    [[nodiscard]] bool failed() const { return failed_; }
private:
    AddressSpace& memory_;
    GuestReadVector scalar_;
    std::span<const GuestReadVector> vectors_;
    GuestReadBuffer buffer_;
    bool failed_ {};
};
} // namespace ilemu
