// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Manage host special-port slots and their kernel-held send rights.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.1.97/osfmk/mach/host_priv.defs

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"
#include "mach/host_priv_mig_ids.hpp"
#include "mach/mig_wire_abi.hpp"

#include "support.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace ilemu {

bool CompatibilityKernel::dispatch_mach_host_special_port_message(
    Cpu& cpu, const MachMessageRequest& request)
{
    namespace mig = xnu::mig::host_priv;
    const auto is_get = request.identifier ==
        mig::id(mig::Routine::host_get_special_port);
    const auto is_set = request.identifier ==
        mig::id(mig::Routine::host_set_special_port);
    if (!is_get && !is_set)
        return false;

    // MIG routine numbers are scoped to their destination service. Bootstrap
    // uses the same range, so leave non-host messages on the normal IPC path.
    {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto object = mach_support::resolve_name_with_right(
            *shared_state_, process_.pid, request.remote_port,
            xnu::ipc::Right::Send);
        if (!object ||
            *object != mach_task_identity::initial_host_self_name)
            return false;
    }

    auto& registers = cpu.registers();
    const auto write_reply = [&](std::uint32_t result,
                                 std::uint32_t port_name = 0) {
        const bool complex = is_get && result == darwin::mach::success;
        const std::array<std::uint32_t, 10> reply {
            darwin::mig_wire::message_bits(
                darwin::mig_wire::disposition_move_send_once, 0, complex),
            complex ? 40U : 36U,
            request.local_port,
            0,
            0,
            request.identifier + 100U,
            complex ? 1U : 0U,
            complex ? port_name : 1U,
            complex ? 0U : result,
            darwin::mig_wire::port_descriptor_metadata(
                darwin::mig_wire::disposition_move_send),
        };
        const auto count = complex ? 10U : 9U;
        for (std::uint32_t index = 0; index < count; ++index) {
            if (!memory_.write32(request.address + index * 4U,
                    reply[index])) {
                registers[0] = darwin::mach_message::receive_invalid_data;
                return;
            }
        }
        registers[0] = darwin::mach::success;
    };
    if (registers[3] < (is_get ? 40U : 36U)) {
        registers[0] = darwin::mach_message::receive_invalid_data;
        return true;
    }
    if (registers[2] < (is_get ? 40U : 52U)) {
        write_reply(darwin::mach::invalid_argument);
        return true;
    }

    const auto which_offset = is_get
                                  ? mig::host_get_special_port_arguments[2]
                                        .request_offset
                                  : mig::host_set_special_port_arguments[1]
                                        .request_offset;
    const auto which = memory_.read32(request.address + which_offset);
    const auto node = is_get
                          ? memory_.read32(request.address +
                                mig::host_get_special_port_arguments[1]
                                    .request_offset)
                          : std::optional<std::uint32_t> { 0 };
    if (!which || !node || *node != 0 || *which == 0 || *which > 255U) {
        write_reply(darwin::mach::invalid_argument);
        return true;
    }

    std::uint32_t result = darwin::mach::success;
    std::uint32_t port_name = 0;
    {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto host_object = mach_support::resolve_name_with_right(
            *shared_state_, process_.pid, request.remote_port,
            xnu::ipc::Right::Send);
        if (!host_object ||
            *host_object != mach_task_identity::initial_host_self_name) {
            result = darwin::mach::invalid_argument;
        } else if (is_get) {
            const auto found = shared_state_->host_special_ports.find(*which);
            if (found != shared_state_->host_special_ports.end()) {
                port_name = shared_state_->mach_namespaces
                                .copyout(process_.pid, found->second,
                                    xnu::ipc::type_mask(
                                        xnu::ipc::Right::Send))
                                .value_or(0);
                if (port_name == 0)
                    result = darwin::mach::resource_shortage;
            }
        } else {
            const auto descriptor_count = memory_.read32(request.address +
                darwin::mig_wire::complex_descriptor_count_offset);
            const auto input_name = memory_.read32(request.address +
                mig::host_set_special_port_arguments[2].request_offset);
            const auto metadata = memory_.read32(request.address +
                darwin::mig_wire::descriptor_metadata_offset(0));
            const auto disposition = metadata
                                         ? (*metadata >>
                                               darwin::mig_wire::
                                                   descriptor_disposition_shift) &
                                               0xffU
                                         : 0U;
            const auto input_object = input_name && *input_name == 0U
                                          ? std::optional<std::uint32_t> { 0 }
                                      : input_name
                                          ? mach_support::resolve_name_with_right(
                                                *shared_state_, process_.pid,
                                                *input_name,
                                                xnu::ipc::Right::Send)
                                          : std::nullopt;
            if ((request.bits & darwin::mig_wire::message_complex_bit) == 0 ||
                descriptor_count != 1 || !metadata || !input_object ||
                ((*metadata >> darwin::mig_wire::descriptor_type_shift) !=
                    darwin::mig_wire::port_descriptor_type) ||
                (disposition != darwin::mig_wire::disposition_move_send &&
                    disposition != darwin::mig_wire::disposition_copy_send)) {
                result = darwin::mach::invalid_capability;
            } else {
                const auto previous = shared_state_->host_special_ports.find(*which);
                const auto prior_object =
                    previous == shared_state_->host_special_ports.end()
                        ? 0U
                        : previous->second;
                if (disposition == darwin::mig_wire::disposition_move_send &&
                    *input_object != 0 &&
                    !mach_support::consume_moved_right_locked(*shared_state_, process_.pid,
                        *input_name, xnu::ipc::Right::Send, true)) {
                    result = darwin::mach::invalid_right;
                } else {
                    if (*input_object != 0 && *input_object != prior_object)
                        mach_support::retain_kernel_send_right_locked(
                            *shared_state_, *input_object);
                    if (*input_object == 0)
                        shared_state_->host_special_ports.erase(*which);
                    else
                        shared_state_->host_special_ports[*which] = *input_object;
                    if (prior_object != 0 && prior_object != *input_object)
                        mach_support::release_kernel_send_right_locked(
                            *shared_state_, prior_object);
                }
            }
        }
    }
    write_reply(result, port_name);
    return true;
}

} // namespace ilemu
