// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Decode guest Mach descriptor kinds, dispositions and memory
// ownership flags.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/message.h

#include "kernel/mach_descriptor_transport.hpp"

#include "kernel/darwin_abi.hpp"

#include "mach/mig_wire_abi.hpp"

#include <algorithm>
#include <utility>

namespace ilemu::mach_transport {
namespace {

    std::uint32_t read_word(
        std::span<const std::byte> bytes, std::size_t offset)
    {
        if (offset + darwin::mig_wire::word_size > bytes.size())
            return 0;
        std::uint32_t result = 0;
        for (std::size_t byte = 0; byte < darwin::mig_wire::word_size; ++byte) {
            result |= std::to_integer<std::uint32_t>(bytes[offset + byte])
                      << (byte * 8U);
        }
        return result;
    }

    DescriptorKind descriptor_kind(std::uint32_t metadata)
    {
        switch (metadata >> darwin::mig_wire::descriptor_type_shift) {
        case darwin::mig_wire::port_descriptor_type:
            return DescriptorKind::Port;
        case darwin::mig_wire::ool_descriptor_type:
        case darwin::mig_wire::ool_volatile_descriptor_type:
            return DescriptorKind::OutOfLineMemory;
        case darwin::mig_wire::ool_ports_descriptor_type:
            return DescriptorKind::OutOfLinePorts;
        default:
            return DescriptorKind::Unknown;
        }
    }

} // namespace

bool Descriptor::deallocate() const
{
    return (metadata & darwin::mig_wire::descriptor_deallocate_mask) != 0;
}

std::uint32_t Descriptor::disposition() const
{
    return (metadata >> darwin::mig_wire::descriptor_disposition_shift) & 0xffU;
}

namespace {

    CopyinDescriptorTable read_descriptors(
        std::span<const std::byte> message, bool copyin)
    {
        using namespace darwin::mig_wire;
        if (message.size() < message_header_size)
            return { { }, darwin::mach_message::send_message_too_small };
        const auto bits = read_word(message, header_bits_offset);
        if ((bits & message_complex_bit) == 0)
            return { };
        if (message.size() < complex_descriptor_base) {
            // Legacy ipc_kmsg_get supplies a zero trailer/count after a
            // header-only complex message (XNU 792 through 2782).
            if (copyin && message.size() == message_header_size)
                return { };
            return { { }, darwin::mach_message::send_message_too_small };
        }

        const auto count = read_word(message, complex_descriptor_count_offset);
        const auto available =
            (message.size() - complex_descriptor_base) / descriptor_size;
        CopyinDescriptorTable result;
        result.descriptors.reserve(std::min<std::size_t>(count, available));
        for (std::uint32_t index = 0; index < count; ++index) {
            // Bounds and copy options are checked together, so an earlier
            // invalid copy option wins over a later truncated descriptor.
            if (index >= available)
                return { { }, darwin::mach_message::send_message_too_small };
            const auto offset = descriptor_offset(index);
            const auto metadata = read_word(message, descriptor_metadata_offset(index));
            const auto kind = descriptor_kind(metadata);
            if ((!copyin && kind == DescriptorKind::Unknown) ||
                (copyin && kind == DescriptorKind::OutOfLineMemory &&
                    ((metadata >> descriptor_copy_shift) & 0xffU) > 1U)) {
                return { { }, darwin::mach_message::send_invalid_type };
            }
            result.descriptors.push_back(Descriptor { kind, offset,
                read_word(message, offset), read_word(message, offset + word_size),
                metadata });
        }
        return result;
    }

} // namespace

CopyinDescriptorTable preflight_copyin_descriptors(
    std::span<const std::byte> message)
{
    return read_descriptors(message, true);
}

std::optional<std::vector<Descriptor>> parse_descriptors(
    std::span<const std::byte> message)
{
    auto result = read_descriptors(message, false);
    if (result.error != 0U)
        return std::nullopt;
    return std::move(result.descriptors);
}

} // namespace ilemu::mach_transport
