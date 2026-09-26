// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "device_state/darwin_abi.hpp"
#include "foundation/address_space.hpp"
#include "foundation/darwin_errno.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ilemu::darwin::sysctl {

// XNU runs the handler (old data, then new data) before copying oldlenp
// back to user space. Keep that final copy separate: it may fail after a
// successful state change, and old/new user buffers are allowed to alias.
class UserTransfer {
public:
    UserTransfer(AddressSpace& memory, std::uint32_t output,
        std::uint32_t length_address, std::uint32_t capacity,
        DarwinSysctlTransferAbi abi, bool kernel_leaf)
        : memory_(memory), output_(output), length_address_(length_address),
          capacity_(capacity), abi_(abi),
          legacy_leaf_(kernel_leaf &&
              abi == DarwinSysctlTransferAbi::LegacyKernelHandlers),
          required_(legacy_leaf_ ? capacity : 0)
    {
    }

    [[nodiscard]] bool legacy_leaf() const { return legacy_leaf_; }
    [[nodiscard]] bool accepts(std::size_t length) const
    {
        return output_ == 0 || capacity_ >= length;
    }
    [[nodiscard]] bool has_input(
        std::uint32_t address, std::uint32_t length) const
    {
        return address != 0 && (legacy_leaf_ || length != 0);
    }

    [[nodiscard]] std::uint32_t write(std::span<const std::byte> bytes)
    {
        if (!accepts(bytes.size()))
            return error::no_memory;
        required_ = static_cast<std::uint32_t>(bytes.size());
        if (output_ == 0)
            return 0;
        return copy_to_user(output_, bytes) ? 0 : error::bad_address;
    }

    [[nodiscard]] std::uint32_t write_string(
        std::string_view value, bool truncate = false)
    {
        if (truncate && output_ != 0 && capacity_ != 0 &&
            capacity_ <= value.size()) {
            // Legacy sysctl_trstring reports the supplied capacity and
            // writes a NUL one byte beyond it. The OID handler includes
            // the terminator within the supplied capacity.
            value = value.substr(0, capacity_ - (legacy_leaf_ ? 0U : 1U));
        }
        if (!accepts(truncate && legacy_leaf_ ? 1U : value.size() + 1U))
            return error::no_memory;
        required_ = static_cast<std::uint32_t>(value.size() + 1U);
        if (truncate && legacy_leaf_)
            --required_;
        if (output_ == 0)
            return 0;
        const auto end = static_cast<std::uint64_t>(output_) + value.size();
        if (end > UINT32_MAX)
            return error::bad_address;
        // Reuse typed guest writes for the terminator. A single payload
        // span takes copy_in's existing fast path without batch vectors.
        if (!copy_to_user(output_,
                std::as_bytes(std::span { value.data(), value.size() })))
            return error::bad_address;
        return memory_.write8(static_cast<std::uint32_t>(end), 0)
            ? 0 : error::bad_address;
    }

    template <class Integer>
    [[nodiscard]] std::uint32_t write_integer(Integer value)
    {
        static_assert(sizeof(Integer) == 4 || sizeof(Integer) == 8);
        if (!accepts(sizeof(Integer)))
            return error::no_memory;
        required_ = sizeof(Integer);
        if (output_ == 0)
            return 0;
        // Preserve the address space's optimized scalar write path. Bulk
        // copying here adds page bookkeeping to every hot numeric query.
        if constexpr (sizeof(Integer) == 4)
            return memory_.write32(output_, static_cast<std::uint32_t>(value))
                ? 0 : error::bad_address;
        else
            return memory_.write64(output_, static_cast<std::uint64_t>(value))
                ? 0 : error::bad_address;
    }

    [[nodiscard]] std::uint32_t write_number64(std::uint64_t value)
    {
        if (abi_ != DarwinSysctlTransferAbi::LegacyKernelHandlers &&
            capacity_ == sizeof(std::uint32_t) &&
            (output_ != 0 || abi_ == DarwinSysctlTransferAbi::OidSizedQueries)) {
            const auto narrowed = static_cast<std::int32_t>(value);
            if (static_cast<std::uint64_t>(static_cast<std::int64_t>(narrowed))
                != value)
                return error::result_too_large;
            return write_integer(static_cast<std::uint32_t>(narrowed));
        }
        return write_integer(value);
    }

    [[nodiscard]] std::uint32_t read_number32(std::uint32_t address,
        std::uint32_t length, std::uint32_t& value) const
    {
        if (legacy_leaf_ ? length != sizeof(value) : length < sizeof(value))
            return error::invalid_argument;
        if (!legacy_leaf_ && length == sizeof(std::uint64_t)) {
            const auto wide = memory_.read64(address);
            if (!wide)
                return error::bad_address;
            const auto narrowed = static_cast<std::int32_t>(*wide);
            if (static_cast<std::uint64_t>(static_cast<std::int64_t>(narrowed))
                != *wide)
                return error::result_too_large;
            value = static_cast<std::uint32_t>(narrowed);
            return 0;
        }
        const auto input = memory_.read32(address);
        if (!input)
            return error::bad_address;
        value = *input;
        return 0;
    }

    [[nodiscard]] std::uint32_t finish(std::uint32_t result)
    {
        if (result != 0 && result != error::no_memory)
            return result;
        if (length_address_ != 0 &&
            !memory_.write32(length_address_, required_))
            return error::bad_address;
        // XNU 1228-4903 assign suulong's return value over ENOMEM. The
        // legacy wrapper preserves the handler error instead.
        if (length_address_ != 0 &&
            abi_ != DarwinSysctlTransferAbi::LegacyKernelHandlers)
            return 0;
        return result;
    }

private:
    [[nodiscard]] bool copy_to_user(
        std::uint32_t address, std::span<const std::byte> bytes)
    {
        // copy_in also serves the loader and intentionally bypasses write
        // permissions; syscall copyout must check the guest mapping first.
        return bytes.empty() ||
            (memory_.accessible(address, bytes.size(), MemoryPermission::Write) &&
                memory_.copy_in(address, bytes));
    }

    AddressSpace& memory_;
    std::uint32_t output_;
    std::uint32_t length_address_;
    std::uint32_t capacity_;
    DarwinSysctlTransferAbi abi_;
    bool legacy_leaf_;
    std::uint32_t required_;
};

} // namespace ilemu::darwin::sysctl
