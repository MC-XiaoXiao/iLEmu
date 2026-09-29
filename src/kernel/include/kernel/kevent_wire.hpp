// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "foundation/address_space.hpp"
#include "kernel/darwin_kqueue_abi.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace ilemu {

struct KeventValue {
    std::uint64_t ident { };
    std::int16_t filter { };
    std::uint16_t flags { };
    std::uint32_t filter_flags { };
    std::int64_t data { };
    std::uint64_t user_data { };
    std::array<std::uint64_t, 2> extension { };
};

// Event formats share knote state and readiness. XNU 3248's kevent_qos_s
// changes offsets as well as size; it is not a kevent64_s with a suffix.
class KeventWireFormat {
public:
    using Format = darwin::kqueue::WireFormat;
    explicit constexpr KeventWireFormat(Format format)
        : format_ { format }
    {
    }
    explicit constexpr KeventWireFormat(bool extended)
        : KeventWireFormat(extended ? Format::Extended64 : Format::Legacy32)
    {
    }

    constexpr std::uint32_t size() const
    {
        return qos() ? 72U : wide() ? 48U : 20U;
    }
    std::optional<KeventValue> read(
        const AddressSpace& memory, std::uint32_t address) const
    {
        if (!memory.accessible(address, size(), MemoryPermission::Read))
            return std::nullopt;
        KeventValue value;
        value.ident =
            wide() ? *memory.read64(address) : *memory.read32(address);
        value.filter = static_cast<std::int16_t>(
            *memory.read16(address + filter_offset()));
        value.flags = *memory.read16(address + filter_offset() + 2U);
        value.filter_flags = *memory.read32(address + flags_offset());
        value.data = wide() ? static_cast<std::int64_t>(
                                  *memory.read64(address + data_offset()))
                            : static_cast<std::int32_t>(
                                  *memory.read32(address + data_offset()));
        value.user_data = wide() ? *memory.read64(address + user_offset())
                                 : *memory.read32(address + user_offset());
        for (std::uint32_t i = 0; i < extension_count(); ++i)
            value.extension[i] =
                *memory.read64(address + extension_offset() + i * 8U);
        return value;
    }
    bool write(AddressSpace& memory, std::uint32_t address,
        const KeventValue& value) const
    {
        if (!memory.accessible(address, size(), MemoryPermission::Write))
            return false;
        if (wide()) {
            if (!memory.write64(address, value.ident) ||
                !memory.write64(address + data_offset(),
                    static_cast<std::uint64_t>(value.data)) ||
                !memory.write64(address + user_offset(), value.user_data))
                return false;
        } else if (!memory.write32(
                       address, static_cast<std::uint32_t>(value.ident)) ||
                   !memory.write32(address + data_offset(),
                       static_cast<std::uint32_t>(value.data)) ||
                   !memory.write32(address + user_offset(),
                       static_cast<std::uint32_t>(value.user_data))) {
            return false;
        }
        for (std::uint32_t i = 0; i < extension_count(); ++i) {
            if (!memory.write64(
                    address + extension_offset() + i * 8U, value.extension[i]))
                return false;
        }
        // XNU 3248's descriptor-backed kqueue copies only ext[0..1]; QoS,
        // xflags and the other extension words are reserved and zeroed.
        if (qos() && (!memory.write32(address + 12U, 0U) ||
                         !memory.write32(address + 28U, 0U) ||
                         !memory.write64(address + 56U, 0U) ||
                         !memory.write64(address + 64U, 0U)))
            return false;
        return memory.write16(address + filter_offset(),
                   static_cast<std::uint16_t>(value.filter)) &&
               memory.write16(address + filter_offset() + 2U, value.flags) &&
               memory.write32(address + flags_offset(), value.filter_flags);
    }

private:
    constexpr bool wide() const { return format_ != Format::Legacy32; }
    constexpr bool qos() const { return format_ == Format::QualityOfService; }
    constexpr std::uint32_t filter_offset() const { return wide() ? 8U : 4U; }
    constexpr std::uint32_t flags_offset() const
    {
        return qos() ? 24U : wide() ? 12U : 8U;
    }
    constexpr std::uint32_t data_offset() const
    {
        return qos() ? 32U : wide() ? 16U : 12U;
    }
    constexpr std::uint32_t user_offset() const
    {
        return qos() ? 16U : wide() ? 24U : 16U;
    }
    constexpr std::uint32_t extension_offset() const
    {
        return qos() ? 40U : 32U;
    }
    constexpr std::uint32_t extension_count() const { return wide() ? 2U : 0U; }
    Format format_;
};

} // namespace ilemu
