// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/darwin_abi.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mach_scheduler_abi.hpp"
#include "network/darwin_network_abi.hpp"
#include "support.hpp"

#include <algorithm>
#include <limits>

namespace ilemu {

void CompatibilityKernel::dispatch_apple80211_ioctl(
    Cpu& cpu, std::uint32_t fd, std::string_view name)
{
    auto& registers = cpu.registers();
    namespace wifi_driver = darwin::network::apple80211_driver;
    constexpr auto wifi_operation_completion_delay =
        10U * darwin::mach::scheduler::nanoseconds_per_millisecond;
    const auto command =
        memory_.read32(registers[2] + wifi_driver::command_offset);
    const auto data_length =
        memory_.read32(registers[2] + wifi_driver::data_length_offset);
    const auto data_address =
        memory_.read32(registers[2] + wifi_driver::data_address_offset);
    if (!command || !data_length || !data_address) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    bool service_available = false;
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        service_available = shared_state_->wifi_service_available;
    }
    if (name != "en0" || !service_available) {
        output_.write(
            "[wifi-driver] reject pid=" + std::to_string(process_.pid) +
            " fd=" + std::to_string(fd) + " interface=" + std::string { name } +
            "\n");
        bsd_error(cpu, 6); // ENXIO
        return;
    }
    if (registers[1] == wifi_driver::set_request &&
        *command == wifi_driver::command_disassociate) {
        const auto before = wifi_state_->snapshot();
        static_cast<void>(wifi_state_->disassociate());
        const auto after = wifi_state_->snapshot();
        apply_wifi_transition(before, after);
        apple80211_hle_.publish_state_change(before, after);
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::set_request &&
        *command == wifi_driver::command_associate) {
        if (*data_address == 0 ||
            *data_length < wifi_driver::association_header_size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto length = memory_.read32(
            *data_address + wifi_driver::association_ssid_length_offset);
        const auto authentication = memory_.read16(*data_address + 6);
        const auto security = memory_.read16(*data_address + 8);
        const auto bssid = memory_.read_bytes(
            *data_address + wifi_driver::association_bssid_offset, 6);
        if (!length || !authentication || !security || !bssid) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        if (*length == 0 || *length > wifi_driver::current_ssid_size ||
            *authentication != 1 || *security != 0) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        const auto bytes = memory_.read_bytes(
            *data_address + wifi_driver::association_ssid_offset, *length);
        if (!bytes) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        const std::string ssid(
            reinterpret_cast<const char*>(bytes->data()), bytes->size());
        const auto before = wifi_state_->snapshot();
        const bool any_bssid = std::all_of(bssid->begin(), bssid->end(),
            [](std::byte value) { return value == std::byte { 0 }; });
        const auto access_points = wifi_state_->scan();
        const auto access_point = std::find_if(access_points.begin(),
            access_points.end(), [&](const auto& candidate) {
                return candidate.ssid == ssid &&
                       (any_bssid || std::equal(bssid->begin(), bssid->end(),
                                         candidate.bssid.begin()));
            });
        if (access_point == access_points.end() ||
            !wifi_state_->associate(ssid)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        const auto after = wifi_state_->snapshot();
        apply_wifi_transition(before, after);
        apple80211_hle_.publish_state_change(before, after);
        const auto now = shared_state_->clock.now();
        const auto deadline =
            wifi_operation_completion_delay >
                    std::numeric_limits<std::uint64_t>::max() - now
                ? std::numeric_limits<std::uint64_t>::max()
                : now + wifi_operation_completion_delay;
        scheduled_wifi_driver_events_.emplace(
            deadline, wifi_driver::event_association_completed);
        output_.write("[wifi-driver] associate ssid=" + ssid + "\n");
        bsd_success(cpu, 0);
        return;
    }
    const auto record_layout =
        shared_state_->darwin_abi.apple80211_ioctl ==
                DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord
            ? wifi_driver::compact_network_record_layout
            : wifi_driver::aligned_network_record_layout;
    if (registers[1] == wifi_driver::set_request &&
        *command == wifi_driver::command_scan) {
        apple80211_scan_delivered_.erase(fd);
        static_cast<void>(wifi_state_->scan());
        const auto now = shared_state_->clock.now();
        const auto deadline =
            wifi_operation_completion_delay >
                    std::numeric_limits<std::uint64_t>::max() - now
                ? std::numeric_limits<std::uint64_t>::max()
                : now + wifi_operation_completion_delay;
        scheduled_wifi_driver_events_.emplace(
            deadline, wifi_driver::event_scan_completed);
        output_.write(
            "[wifi-driver] scan-start pid=" + std::to_string(process_.pid) +
            " fd=" + std::to_string(fd) + "\n");
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::set_request &&
        *command == wifi_driver::command_power) {
        if (*data_address == 0 ||
            *data_length < wifi_driver::power_state_size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto count = memory_.read32(
            *data_address + wifi_driver::power_state_count_offset);
        const auto power = memory_.read32(
            *data_address + wifi_driver::power_state_first_value_offset);
        if (!count || !power || *count == 0) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        const auto before = wifi_state_->snapshot();
        static_cast<void>(wifi_state_->set_power(*power != 0));
        const auto after = wifi_state_->snapshot();
        apply_wifi_transition(before, after);
        apple80211_hle_.publish_state_change(before, after);
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_interface_probe) {
        output_.write(
            "[wifi-driver] probe pid=" + std::to_string(process_.pid) +
            " fd=" + std::to_string(fd) + " interface=en0\n");
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_capability_probe &&
        *data_length == 0U) {
        output_.write("[wifi-driver] capability-probe pid=" +
                      std::to_string(process_.pid) +
                      " fd=" + std::to_string(fd) + "\\n");
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_power) {
        if (*data_address == 0 ||
            *data_length < wifi_driver::power_state_size ||
            !memory_.write32(
                *data_address + wifi_driver::power_state_count_offset, 1) ||
            !memory_.write32(
                *data_address + wifi_driver::power_state_first_value_offset,
                wifi_state_->snapshot().powered ? 1U : 0U)) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_state) {
        if (!memory_.write32(
                registers[2] + wifi_driver::inline_scalar_value_offset,
                wifi_state_->snapshot().associated_access_point ? 1U : 0U)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_association_result) {
        if (!wifi_state_->snapshot().associated_access_point) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
        }
        if (!memory_.write32(
                registers[2] + wifi_driver::inline_scalar_value_offset,
                wifi_driver::association_result_success)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_rate) {
        const auto associated = wifi_state_->snapshot().associated_access_point;
        if (!associated) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
        }
        if (!memory_.write32(
                registers[2] + wifi_driver::inline_scalar_value_offset,
                associated->link_rate_mbps)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_current_ssid) {
        if (*data_address == 0 ||
            *data_length < wifi_driver::current_ssid_size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto associated = wifi_state_->snapshot().associated_access_point;
        if (!associated) {
            // A successful command means that an association
            // exists. Returning an empty successful payload makes
            // legacy Apple80211 clients construct a non-null
            // "current network" object from a scan result.
            bsd_error(cpu, 57); // ENOTCONN
            return;
        }
        const auto copied = std::min<std::size_t>(
            associated->ssid.size(), wifi_driver::current_ssid_size);
        std::array<std::byte, wifi_driver::current_ssid_size> ssid { };
        std::transform(associated->ssid.begin(),
            associated->ssid.begin() + copied, ssid.begin(), [](char value) {
                return static_cast<std::byte>(
                    static_cast<unsigned char>(value));
            });
        if (!memory_.copy_in(
                *data_address, std::span { ssid.data(), copied }) ||
            !memory_.write32(registers[2] + wifi_driver::data_length_offset,
                static_cast<std::uint32_t>(copied))) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_current_bssid) {
        if (*data_address == 0 ||
            *data_length < wifi_driver::current_bssid_size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto associated = wifi_state_->snapshot().associated_access_point;
        if (!associated) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
        }
        if (!memory_.copy_in(
                *data_address, std::span { associated->bssid.data(),
                                   associated->bssid.size() })) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_supported_channels) {
        constexpr std::uint32_t channel_count = 11;
        constexpr std::uint32_t channel_2ghz_flag = 1U << 3U;
        constexpr auto size = wifi_driver::channel_list_header_size +
                              channel_count * wifi_driver::channel_record_size;
        if (*data_address == 0 || *data_length < size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        // Advertise the virtual radio's complete 2.4 GHz channel range,
        // independently of the channel currently occupied by its AP.
        std::array<std::uint32_t, size / sizeof(std::uint32_t)> channels { };
        channels[0] = 1;
        channels[1] = channel_count;
        for (std::uint32_t index = 0; index < channel_count; ++index) {
            channels[2 + index * 3] = 1;
            channels[3 + index * 3] = index + 1;
            channels[4 + index * 3] = channel_2ghz_flag;
        }
        for (std::size_t index = 0; index < channels.size(); ++index) {
            if (!memory_.write32(*data_address +
                    static_cast<std::uint32_t>(index * sizeof(std::uint32_t)),
                    channels[index])) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_channel) {
        if (*data_address == 0 ||
            *data_length < wifi_driver::channel_state_size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto associated = wifi_state_->snapshot().associated_access_point;
        if (!associated) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
        }
        if (!memory_.write32(
                *data_address + wifi_driver::channel_state_channel_offset,
                associated->channel) ||
            !memory_.write32(
                *data_address + wifi_driver::channel_state_flags_offset, 0)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        (*command == wifi_driver::command_rssi ||
            *command == wifi_driver::command_noise)) {
        if (*data_address == 0 ||
            *data_length < wifi_driver::signal_state_size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto associated = wifi_state_->snapshot().associated_access_point;
        if (!associated) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
        }
        const auto signal = static_cast<std::uint32_t>(
            *command == wifi_driver::command_rssi ? associated->rssi : -90);
        if (!memory_.write32(
                *data_address + wifi_driver::signal_state_count_offset, 1) ||
            !memory_.write32(
                *data_address + wifi_driver::signal_state_unit_offset, 0) ||
            !memory_.write32(
                *data_address +
                    wifi_driver::signal_state_control_average_offset,
                signal) ||
            !memory_.write32(
                *data_address +
                    wifi_driver::signal_state_extension_average_offset,
                signal) ||
            !memory_.write32(
                *data_address + wifi_driver::signal_state_last_offset,
                signal)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_driver_name) {
        if (*data_address == 0 || *data_length == 0) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        constexpr auto driver_name = wifi_driver::event_device_name;
        std::vector<std::byte> name_buffer(
            std::min<std::size_t>(*data_length, driver_name.size() + 1),
            std::byte { 0 });
        std::transform(driver_name.begin(),
            driver_name.begin() +
                std::min(driver_name.size(), name_buffer.size() - 1),
            name_buffer.begin(), [](char value) {
                return static_cast<std::byte>(
                    static_cast<unsigned char>(value));
            });
        if (!memory_.copy_in(*data_address, name_buffer)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        bsd_success(cpu, 0);
        return;
    }
    if (registers[1] == wifi_driver::get_request &&
        *command == wifi_driver::command_scan_result) {
        if (apple80211_scan_delivered_.contains(fd)) {
            output_.write(
                "[wifi-driver] scan-end pid=" + std::to_string(process_.pid) +
                " fd=" + std::to_string(fd) + "\n");
            bsd_error(cpu,
                5); // EIO terminates the firmware scan iterator.
            return;
        }
        const auto access_points = wifi_state_->scan();
        if (access_points.empty()) {
            bsd_error(cpu, 5);
            return;
        }
        const auto& access_point = access_points.front();
        if (*data_address == 0 || *data_length < record_layout.size ||
            access_point.ssid.size() > 32) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto ie_scratch =
            memory_.read32(*data_address + record_layout.ie_pointer_offset);
        if (!ie_scratch) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        // Preserve only the firmware-owned IE scratch pointer.
        // Copying the entire input record would leak uninitialized
        // caller flags into the native parser and can make a
        // discovered AP appear associated.
        const auto result = wifi_driver::make_network_record(
            &access_point, record_layout, 0, *ie_scratch);
        if (!result || !memory_.copy_in(*data_address, *result)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        apple80211_scan_delivered_.insert(fd);
        output_.write(
            "[wifi-driver] scan-result pid=" + std::to_string(process_.pid) +
            " fd=" + std::to_string(fd) + " ssid=" + access_point.ssid + "\n");
        bsd_success(cpu, 0);
        return;
    }
    // Both public-family and embedded driver selectors carry
    // the network record described by the selected wire contract.
    constexpr std::uint32_t embedded_current_network = 103;
    if (registers[1] == wifi_driver::get_request &&
        (*command == wifi_driver::command_current_network ||
            *command == embedded_current_network)) {
        const auto layout = record_layout;
        if (*data_address == 0 || *data_length < layout.size) {
            bsd_error(cpu, *data_address == 0 ? bsd_support::bad_address
                                              : bsd_support::invalid_argument);
            return;
        }
        const auto ie_length =
            memory_.read16(*data_address + layout.ie_length_offset);
        const auto ie_pointer =
            memory_.read32(*data_address + layout.ie_pointer_offset);
        if (!ie_length || !ie_pointer ||
            (*ie_length != 0 && *ie_pointer == 0)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        const auto associated = wifi_state_->snapshot().associated_access_point;
        // The embedded current-BSS query reports absence as ENOTCONN.
        // The older current-network snapshot selector returns an empty
        // record: its clients still serialize that snapshot when unassociated.
        if (!associated && *command == embedded_current_network) {
            bsd_error(cpu, 57); // ENOTCONN: no current BSS.
            return;
        }
        const auto record = wifi_driver::make_network_record(
            associated ? &*associated : nullptr, layout, 0, *ie_pointer);
        if (!record || !memory_.copy_in(*data_address, *record)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        output_.write(
            "[wifi-driver] current-network pid=" +
            std::to_string(process_.pid) + " fd=" + std::to_string(fd) +
            " layout=" + std::to_string(layout.size) +
            " associated=" + std::to_string(associated.has_value()) + "\n");
        bsd_success(cpu, 0);
        return;
    }
    output_.write(
        "[wifi-driver] unsupported pid=" + std::to_string(process_.pid) +
        " fd=" + std::to_string(fd) + " operation=" +
        (registers[1] == wifi_driver::get_request ? "get" : "set") +
        " command=" + std::to_string(*command) +
        " length=" + std::to_string(*data_length) + "\n");
    bsd_error(cpu, bsd_support::invalid_argument);
    return;
}

} // namespace ilemu
