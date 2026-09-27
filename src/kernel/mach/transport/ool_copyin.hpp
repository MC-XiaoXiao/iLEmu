// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "../support.hpp"
#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/mach_descriptor_transport.hpp"
#include "mach/mig_wire_abi.hpp"

namespace ilemu::mach_transport {

// Copy once before releasing the source pages. Port-array names must remain
// available after deallocation, including when a later element is invalid.
class OolCopyin {
public:
    struct Result {
        std::uint32_t error { };
        std::vector<std::byte> bytes;
    };
    explicit OolCopyin(AddressSpace& memory) : memory_(memory) { }
    [[nodiscard]] Result capture(const Descriptor& descriptor) const
    {
        const auto ports = descriptor.kind == DescriptorKind::OutOfLinePorts;
        const auto limit = ports
            ? mach_support::maximum_message_io / darwin::mig_wire::word_size
            : mach_support::maximum_ool_payload;
        if (descriptor.count_or_size > limit)
            return { darwin::mach_message::send_too_large, { } };
        const auto size = descriptor.count_or_size *
            (ports ? darwin::mig_wire::word_size : 1U);
        if (size == 0)
            return { };
        auto bytes = memory_.read_bytes(descriptor.address_or_name, size);
        if (!bytes)
            return { darwin::mach_message::send_invalid_memory, { } };
        if (descriptor.deallocate())
            static_cast<void>(memory_.unmap(descriptor.address_or_name, size));
        return { 0U, std::move(*bytes) };
    }
private:
    AddressSpace& memory_;
};

} // namespace ilemu::mach_transport
