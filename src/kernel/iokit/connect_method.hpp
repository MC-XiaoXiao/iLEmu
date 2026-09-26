// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Encode profiled IOKit external-method MIG requests and replies.
// XNU device.defs server contracts clamp output capacities, require exact
// compressed request sizes, and return mig_reply_error_t on method failure.
// Message queuing and receive trailers belong to the outer Mach transport.

#pragma once

#include "device_state/darwin_abi.hpp"
#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/iokit_abi.hpp"
#include "mach/mig_wire_abi.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ilemu::kernel_iokit {
class ConnectMethodCodec {
    static constexpr std::uint32_t mach_rcv_invalid_data =
        darwin::mach_message::receive_invalid_data;
    static constexpr std::uint32_t mach_reply_bits = 18;
    static constexpr std::uint32_t mach_ndr_native = 0;
    static constexpr std::uint32_t mach_ndr_little_endian = 1;
    static constexpr std::uint32_t mig_reply_id_delta = 100;

public:
    struct Request {
        struct OutOfLineStructure {
            std::uint64_t address { };
            std::uint64_t size { };
        };

        std::uint32_t selector { };
        std::array<std::uint64_t,
            iokit_abi::connect_method::maximum_scalar_count>
            scalar_input { };
        std::uint32_t scalar_input_count { };
        std::vector<std::byte> inband_input;
        std::uint32_t scalar_output_capacity { };
        std::uint32_t inband_output_capacity { };
        OutOfLineStructure out_of_line_input;
        OutOfLineStructure out_of_line_output;
    };

    struct Result {
        std::uint32_t return_code { iokit_abi::unsupported };
        std::vector<std::uint64_t> scalar_output;
        std::vector<std::byte> inband_output;
    };

    static std::optional<Request> read(AddressSpace& memory,
        std::uint32_t address, std::uint32_t send_size,
        DarwinIOConnectMethodAbi profile, std::uint32_t& error)
    {
        using namespace iokit_abi::connect_method;
        error = darwin::mig::bad_arguments;
        // ipc_kmsg_get validates transfer size and copies the entire request
        // before MIG checks its compressed arrays or calls a user client.
        if (send_size < darwin::mig_wire::message_header_size ||
            send_size % darwin::mig_wire::word_size != 0) {
            error = darwin::mach_message::send_message_too_small;
            return std::nullopt;
        }
        constexpr auto maximum_request_size =
            scalar_input_offset + inband_count_size +
            maximum_scalar_count * scalar_size + maximum_inband_size +
            mach_vm64_trailing_request_size;
        if (static_cast<std::uint64_t>(address) + send_size > (1ULL << 32U)) {
            error = darwin::mach_message::send_invalid_data;
            return std::nullopt;
        }
        if (send_size > maximum_request_size) {
            if (!memory.accessible(address, send_size, MemoryPermission::Read))
                error = darwin::mach_message::send_invalid_data;
            return std::nullopt;
        }
        std::array<std::byte, maximum_request_size> bytes;
        if (!memory.copy_out(address, std::span { bytes }.first(send_size))) {
            error = darwin::mach_message::send_invalid_data;
            return std::nullopt;
        }
        const auto word = [&](std::size_t offset) {
            std::uint32_t value = 0;
            for (std::size_t index = 0; index < sizeof(value); ++index)
                value |= std::to_integer<std::uint32_t>(bytes[offset + index])
                         << (index * 8U);
            return value;
        };
        const auto wide =
            profile == DarwinIOConnectMethodAbi::MachVm64OolStructureThenScalar;
        const auto ool_size =
            wide ? mach_vm64_ool_request_size : natural32_ool_request_size;
        const auto tail_size =
            wide ? mach_vm64_trailing_request_size : trailing_request_size;
        const auto minimum_size =
            scalar_input_offset + inband_count_size + tail_size;
        if ((word(darwin::mig_wire::header_bits_offset) &
                darwin::mig_wire::message_complex_bit) != 0 ||
            send_size < minimum_size)
            return std::nullopt;
        Request request;
        request.selector = word(selector_offset);
        request.scalar_input_count = word(scalar_input_count_offset);
        if (request.scalar_input_count > maximum_scalar_count)
            return std::nullopt;
        const auto scalar_bytes = request.scalar_input_count * scalar_size;
        if (send_size < minimum_size + scalar_bytes)
            return std::nullopt;
        const auto input_offset =
            scalar_input_offset + scalar_bytes + inband_count_size;
        const auto input_count = word(input_offset - inband_count_size);
        if (input_count > maximum_inband_size)
            return std::nullopt;
        const auto tail =
            input_offset + aligned_inband_output_size(input_count);
        if (send_size != tail + tail_size)
            return std::nullopt;
        for (std::uint32_t index = 0; index < request.scalar_input_count;
            ++index) {
            const auto offset = scalar_input_offset + index * scalar_size;
            request.scalar_input[index] =
                static_cast<std::uint64_t>(word(offset)) |
                (static_cast<std::uint64_t>(
                     word(offset + sizeof(std::uint32_t)))
                    << 32U);
        }
        request.inband_input.assign(bytes.begin() + input_offset,
            bytes.begin() + input_offset + input_count);
        const auto vm_size = ool_size / 2U;
        const auto vm_value = [&](std::size_t offset) {
            const auto low = word(tail + offset);
            return static_cast<std::uint64_t>(low) |
                   (wide ? static_cast<std::uint64_t>(word(tail + offset + 4U))
                               << 32U
                         : 0U);
        };
        request.out_of_line_input = { vm_value(0), vm_value(vm_size) };
        const auto output_offset = ool_size + output_capacity_request_size;
        request.out_of_line_output = { vm_value(output_offset),
            vm_value(output_offset + vm_size) };
        const auto first_capacity = word(tail + ool_size);
        const auto second_capacity =
            word(tail + ool_size + sizeof(std::uint32_t));
        const auto scalar_first =
            profile ==
            DarwinIOConnectMethodAbi::Natural32OolScalarThenStructure;
        // MIG initializes server arrays to their fixed maxima and only lowers
        // their counts for smaller client capacities; these are not input
        // counts.
        request.scalar_output_capacity = std::min(maximum_scalar_count,
            scalar_first ? first_capacity : second_capacity);
        request.inband_output_capacity = std::min(maximum_inband_size,
            scalar_first ? second_capacity : first_capacity);
        error = 0;
        return request;
    }

    static std::uint32_t write_error(AddressSpace& memory,
        std::uint32_t address, std::uint32_t local_port,
        std::uint32_t message_id, std::uint32_t receive_size,
        std::uint32_t error)
    {
        // MIG_RETURN_ERROR leaves the initial mig_reply_error_t size intact.
        if (receive_size < darwin::mig_wire::simple_reply_payload_base)
            return mach_rcv_invalid_data;
        const std::array<std::uint32_t, 9> reply { mach_reply_bits,
            darwin::mig_wire::simple_reply_payload_base, local_port, 0, 0,
            message_id + mig_reply_id_delta, mach_ndr_native,
            mach_ndr_little_endian, error };
        for (std::size_t index = 0; index < reply.size(); ++index) {
            if (!memory.write32(
                    address + static_cast<std::uint32_t>(index * 4U),
                    reply[index]))
                return mach_rcv_invalid_data;
        }
        return 0;
    }

    static std::uint32_t write_reply(AddressSpace& memory,
        std::uint32_t address, std::uint32_t local_port,
        std::uint32_t message_id, std::uint32_t receive_size,
        const Result& result, DarwinIOConnectMethodAbi profile,
        std::uint64_t out_of_line_output_size)
    {
        if (result.return_code != iokit_abi::success)
            return write_error(memory, address, local_port, message_id,
                receive_size, result.return_code);
        using namespace iokit_abi::connect_method;
        const auto scalar_count =
            static_cast<std::uint32_t>(std::min<std::size_t>(
                result.scalar_output.size(), maximum_scalar_count));
        const auto inband_count =
            static_cast<std::uint32_t>(std::min<std::size_t>(
                result.inband_output.size(), maximum_inband_size));
        std::uint32_t scalar_count_offset { };
        std::uint32_t scalar_bytes_offset { };
        std::uint32_t inband_count_offset { };
        std::uint32_t inband_bytes_offset { };
        const auto mach_vm64_ool =
            profile == DarwinIOConnectMethodAbi::MachVm64OolStructureThenScalar;
        std::uint32_t ool_output_offset { };
        if (profile !=
            DarwinIOConnectMethodAbi::Natural32OolScalarThenStructure) {
            inband_count_offset = return_code_offset + sizeof(std::uint32_t);
            inband_bytes_offset = inband_count_offset + sizeof(std::uint32_t);
            scalar_count_offset =
                inband_bytes_offset + aligned_inband_output_size(inband_count);
            scalar_bytes_offset = scalar_count_offset + sizeof(std::uint32_t);
            ool_output_offset =
                scalar_bytes_offset + scalar_count * scalar_size;
        } else {
            scalar_count_offset = scalar_output_count_offset;
            scalar_bytes_offset = scalar_output_offset;
            inband_count_offset = inband_output_count_offset(scalar_count);
            inband_bytes_offset = inband_output_offset(scalar_count);
            ool_output_offset =
                out_of_line_output_size_offset(scalar_count, inband_count);
        }
        const auto ool_reply_size =
            mach_vm64_ool ? mach_vm64_ool_reply_size : natural32_ool_reply_size;
        const auto encoded_reply_size =
            static_cast<std::uint32_t>(ool_output_offset + ool_reply_size);
        if (encoded_reply_size > receive_size)
            return mach_rcv_invalid_data;
        const std::vector<std::byte> cleared_reply(encoded_reply_size);
        if (!memory.copy_in(address, cleared_reply))
            return mach_rcv_invalid_data;
        const auto write = [&](std::uint32_t offset, std::uint32_t value) {
            return memory.write32(address + offset, value);
        };
        if (!write(darwin::mig_wire::header_bits_offset, mach_reply_bits) ||
            !write(darwin::mig_wire::header_size_offset, encoded_reply_size) ||
            !write(darwin::mig_wire::header_remote_port_offset, local_port) ||
            !write(darwin::mig_wire::header_identifier_offset,
                message_id + mig_reply_id_delta) ||
            !write(darwin::mig_wire::message_header_size, mach_ndr_native) ||
            !write(darwin::mig_wire::message_header_size +
                       darwin::mig_wire::word_size,
                mach_ndr_little_endian) ||
            !write(return_code_offset, result.return_code) ||
            !write(scalar_count_offset, scalar_count) ||
            !write(inband_count_offset, inband_count)) {
            return mach_rcv_invalid_data;
        }
        for (std::uint32_t index = 0; index < scalar_count; ++index) {
            const auto scalar_offset =
                scalar_bytes_offset + index * scalar_size;
            if (!write(scalar_offset,
                    static_cast<std::uint32_t>(result.scalar_output[index])) ||
                !write(scalar_offset + sizeof(std::uint32_t),
                    static_cast<std::uint32_t>(
                        result.scalar_output[index] >> 32U))) {
                return mach_rcv_invalid_data;
            }
        }
        if (inband_count != 0 &&
            !memory.copy_in(address + inband_bytes_offset,
                std::span<const std::byte> {
                    result.inband_output.data(), inband_count })) {
            return mach_rcv_invalid_data;
        }
        if (!write(ool_output_offset,
                static_cast<std::uint32_t>(out_of_line_output_size)) ||
            (mach_vm64_ool && !write(ool_output_offset + sizeof(std::uint32_t),
                                  static_cast<std::uint32_t>(
                                      out_of_line_output_size >> 32U)))) {
            return mach_rcv_invalid_data;
        }
        return 0;
    }
};

} // namespace ilemu::kernel_iokit
