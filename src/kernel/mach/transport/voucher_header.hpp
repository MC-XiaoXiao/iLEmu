// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "kernel/darwin_abi.hpp"
#include "../support.hpp"
#include "port_copyin.hpp"

namespace ilemu::mach_transport {

// ipc_kmsg_copyin_header validates vouchers before destination/reply lookup,
// except that literal NULL/DEAD destinations always fail first. This is a
// preflight only: header rights are committed together by the transport.
class VoucherHeader {
public:
    struct Result {
        std::uint32_t error { };
        std::optional<std::uint32_t> object;
    };

    // Caller holds mach_mutex. A zero disposition leaves the opaque header
    // word alone (including the pre-voucher ABI's reserved field).
    [[nodiscard]] static Result validate_locked(const KernelSharedState& state,
        std::uint32_t task, std::uint32_t destination, std::uint32_t reply,
        std::uint32_t bits,
        std::uint32_t name)
    {
        const auto disposition = (bits >> 16U) & 0x1fU;
        if (disposition == 0U)
            return { };
        if (destination == xnu::ipc::null_name || destination == xnu::ipc::dead_name)
            return { darwin::mach_message::send_invalid_destination, { } };
        if (name == xnu::ipc::dead_name ||
            (disposition != 17U && disposition != 19U))
            return { darwin::mach_message::send_invalid_voucher, { } };
        if (name == xnu::ipc::null_name)
            return { };
        const auto object = mach_support::resolve_name_with_right(
            state, task, name, xnu::ipc::Right::Send);
        if (!object || !state.mach_vouchers.contains(*object))
            return { darwin::mach_message::send_invalid_voucher, { } };
        // A voucher cannot also be the reply port. Native copyin checks
        // destination entry existence before reporting this alias error.
        if (reply == name) {
            return { state.mach_namespaces.lookup(task, destination)
                ? darwin::mach_message::send_invalid_reply
                : darwin::mach_message::send_invalid_destination, { } };
        }
        if (destination == name) {
            // Native joint destination/voucher copyin preflights the
            // independent reply before checking the destination pair.
            if (reply != xnu::ipc::null_name && reply != xnu::ipc::dead_name &&
                !PortCopyin { state, task }.capture(reply, (bits >> 8U) & 0x1fU,
                    darwin::mig_wire::header_local_port_offset))
                return { darwin::mach_message::send_invalid_reply, { } };
            const auto destination_type = bits & 0x1fU;
            if (destination_type != 17U && destination_type != 19U)
                return { darwin::mach_message::send_invalid_destination, { } };
            const auto entry = state.mach_namespaces.lookup(task, name);
            if (destination_type == 17U && disposition == 17U &&
                entry->user_references[static_cast<std::size_t>(xnu::ipc::Right::Send)] < 2U)
                return { darwin::mach_message::send_invalid_destination, { } };
        }
        return { 0U, object };
    }
};

} // namespace ilemu::mach_transport
