// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Darwin 14 NECP application policy matching. The compatibility kernel does
// not install NECP policies, so a valid query returns XNU's no-match result.
// ABI: xnu-2782.1.97 bsd/net/necp.h and bsd/net/necp.c.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_network.hpp"
#include "network/necp_policy.hpp"
#include "support.hpp"

#include <algorithm>
#include <charconv>

namespace ilemu {
namespace {

std::vector<darwin::necp::Interface> policy_interfaces(KernelSharedState& state)
{
    std::lock_guard lock { state.network_mutex };
    std::vector<darwin::necp::Interface> result;
    for (const auto& [name, interface] : state.network_interfaces) {
        const auto functional_type =
            interface.family == darwin::network::interface_family_loopback ? 1U :
            interface.media_type == darwin::network::media_type_ieee80211 ? 3U :
            interface.type == darwin::network::interface_type_ethernet ? 2U : 0U;
        result.push_back({ kernel_network::make_interface_snapshot(name, interface),
            functional_type });
    }
    return result;
}

darwin::necp::ClientId policy_client_id()
{
    const auto text = make_host_uuid();
    darwin::necp::ClientId result;
    auto cursor = text.data();
    for (auto& byte : result) {
        if (*cursor == '-')
            ++cursor;
        unsigned value;
        static_cast<void>(std::from_chars(cursor, cursor + 2, value, 16));
        byte = static_cast<std::byte>(value);
        cursor += 2;
    }
    return result;
}

} // namespace

void CompatibilityKernel::refresh_network_policy_clients()
{
    const auto interfaces = policy_interfaces(*shared_state_);
    if (shared_state_->network_policy_clients->refresh(
            [&](const auto& id, auto parameters) {
                return darwin::necp::evaluate(id, parameters, interfaces,
                    shared_state_->route_table);
            }))
        shared_state_->note_io_event_transition();
}

void CompatibilityKernel::dispatch_bsd_network_policy(Cpu& cpu, std::uint32_t number)
{
    const auto& registers = cpu.registers();
    if (number == 501U) {
        const auto fd = allocate_file_descriptor();
        if (!fd) {
            bsd_error(cpu, darwin::error::too_many_open_files);
            return;
        }
        const bool observer = (registers[0] & 1U) != 0U;
        virtual_descriptors_[*fd] = "necp";
        network_policy_descriptors_[*fd] =
            shared_state_->network_policy_clients->open(observer);
        file_status_flags_[*fd] = darwin::open_flag::read_only;
        descriptor_flags_[*fd] = 1U | DarwinFileGuard::descriptor_close_on_fork;
        bsd_success(cpu, *fd);
        return;
    }
    if (number == 502U) {
        const auto found = network_policy_descriptors_.find(registers[0]);
        if (found == network_policy_descriptors_.end()) {
            bsd_error(cpu, darwin::error::bad_file_descriptor);
            return;
        }
        const auto& session = found->second;
        const auto action = registers[1];
        const auto identifier = registers[2];
        const auto identifier_size = registers[3];
        const auto buffer = registers[4];
        const auto capacity = registers[5];
        if ((action == 3U || action == 4U) &&
            (buffer == 0U || capacity == 0U)) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        if (action == 5U) { // COPY_LIST
            if (buffer == 0U || capacity < 4U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            if (!session->observer()) {
                bsd_error(cpu, darwin::error::permission_denied);
                return;
            }
            const auto count = memory_.read32(buffer);
            if (!count) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            if (capacity != 4ULL + *count * 16ULL) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            const auto clients = session->list();
            if (!memory_.write32(buffer, static_cast<std::uint32_t>(clients.size()))) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            for (std::uint32_t i = 0; i < *count; ++i) {
                const darwin::necp::ClientId empty { };
                const auto& id = i < clients.size() ? clients[i] : empty;
                if (!memory_.copy_to_user(buffer + 4U + i * 16U, id)) {
                    bsd_error(cpu, darwin::error::bad_address);
                    return;
                }
            }
            bsd_success(cpu, 0U);
            return;
        }
        if (action == 9U) { // COPY_INTERFACE
            if (identifier == 0U || identifier_size != 4U || buffer == 0U || capacity < 80U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            const auto index = memory_.read32(identifier);
            if (!index) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            if (*index == 0U) {
                bsd_error(cpu, darwin::error::no_entry);
                return;
            }
            const auto result = darwin::necp::interface_details(*index,
                policy_interfaces(*shared_state_));
            if (!memory_.copy_to_user(buffer, result))
                bsd_error(cpu, darwin::error::bad_address);
            else
                bsd_success(cpu, 0U);
            return;
        }
        darwin::necp::ClientId id { };
        if (identifier != 0U) {
            if (identifier_size != id.size()) {
                bsd_error(cpu, action == 3U || action == 4U ?
                    darwin::error::result_too_large : darwin::error::invalid_argument);
                return;
            }
            if (!memory_.copy_out(identifier, id)) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
        } else if (action == 1U || action == 2U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        switch (action) {
        case 1U: { // ADD
            if (buffer == 0U || capacity == 0U || capacity > 1024U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            auto parameters = memory_.read_bytes(buffer, capacity);
            if (!parameters) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            id = policy_client_id();
            if (!memory_.copy_to_user(identifier, id)) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            auto result = darwin::necp::evaluate(id, *parameters,
                policy_interfaces(*shared_state_), shared_state_->route_table);
            session->add(id, std::move(*parameters), std::move(result));
            shared_state_->note_io_event_transition();
            bsd_success(cpu, 0U);
            return;
        }
        case 2U: // REMOVE
            session->remove(id);
            shared_state_->note_io_event_transition();
            bsd_success(cpu, 0U);
            return;
        case 3U: // COPY_PARAMETERS
        case 4U: { // COPY_RESULT
            std::uint32_t error { }, copied { };
            const auto result = session->copy(id, action == 3U, [&](auto bytes) {
                if (bytes.size() > capacity) {
                    error = darwin::error::invalid_argument;
                    return false;
                }
                if (!memory_.copy_to_user(buffer, bytes)) {
                    error = darwin::error::bad_address;
                    return false;
                }
                copied = static_cast<std::uint32_t>(bytes.size());
                return true;
            });
            if (!result)
                bsd_error(cpu, darwin::error::no_entry);
            else if (!*result)
                bsd_error(cpu, error);
            else {
                shared_state_->note_io_event_transition();
                bsd_success(cpu, copied);
            }
            return;
        }
        default:
            trace_unknown(cpu, "NECP client action", action);
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
    }
    // The older necp_match_policy contract has no installed application rules.
    constexpr std::uint32_t maximum_parameters = 1024U;
    constexpr std::uint32_t aggregate_result_words = 10U;
    const auto parameters = registers[0];
    const auto parameter_size = registers[1];
    const auto result = registers[2];
    if (parameters == 0U || parameter_size == 0U ||
        parameter_size > maximum_parameters || result == 0U) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    if (!memory_.read_bytes(parameters, parameter_size)) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    for (std::uint32_t word = 0; word < aggregate_result_words; ++word) {
        if (!memory_.write32(result + word * sizeof(std::uint32_t), 0U)) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
    }
    bsd_success(cpu, 0U);
}

} // namespace ilemu
