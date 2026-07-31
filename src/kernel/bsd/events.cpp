#include "ilemu/kernel.hpp"

#include "ilemu/baseband_device.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/darwin_sysctl.hpp"
#include "ilemu/kernel_network.hpp"
#include "ilemu/offline_serial_device.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace {

[[nodiscard]] bool is_printable_ascii(std::byte value) {
  const auto character = std::to_integer<unsigned char>(value);
  return character >= 0x20U && character <= 0x7eU;
}

[[nodiscard]] std::optional<std::string>
extract_inline_ascii_string(std::span<const std::byte> bytes) {
  for (std::size_t start = 0; start < bytes.size(); ++start) {
    if (!is_printable_ascii(bytes[start])) {
      continue;
    }
    std::string candidate;
    for (std::size_t index = start; index < bytes.size(); ++index) {
      const auto character = std::to_integer<unsigned char>(bytes[index]);
      if (character == 0U) {
        break;
      }
      if (character < 0x20U || character > 0x7eU) {
        candidate.clear();
        break;
      }
      candidate.push_back(static_cast<char>(character));
    }
    if (candidate.size() >= 3U) {
      return candidate;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool is_printable_ascii_string(std::string_view value) {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return character >= 0x20U && character <= 0x7eU;
         });
}

[[nodiscard]] std::optional<std::string>
extract_mux_channel_name(const ilemu::AddressSpace &memory,
                         std::uint32_t base_address,
                         std::span<const std::byte> bytes) {
  if (const auto inline_name = extract_inline_ascii_string(bytes);
      inline_name && is_printable_ascii_string(*inline_name)) {
    return inline_name;
  }
  for (std::size_t offset = 0; offset + sizeof(std::uint32_t) <= bytes.size();
       offset += sizeof(std::uint32_t)) {
    const auto pointer =
        memory.read32(base_address + static_cast<std::uint32_t>(offset));
    if (!pointer || *pointer == 0U) {
      continue;
    }
    const auto candidate = memory.read_c_string(*pointer);
    if (!candidate || !is_printable_ascii_string(*candidate) ||
        candidate->size() < 3U || candidate->size() > 63U) {
      continue;
    }
    return candidate;
  }
  return std::nullopt;
}

} // namespace

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_events(Cpu &cpu, std::uint32_t number) {
  auto &registers = cpu.registers();
  switch (number) {
  case 54: { // ioctl
    const auto fd = registers[0];
    if (const auto block = virtual_block_descriptors_.find(fd);
        block != virtual_block_descriptors_.end()) {
      switch (registers[1]) {
      case 0x40046418U: // DKIOCGETBLOCKSIZE / legacy DKIOCBLKSIZE
        if (!memory_.write32(registers[2], 512)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      case 0x40086419U: { // DKIOCGETBLOCKCOUNT
        std::error_code size_error;
        const auto size =
            std::filesystem::file_size(file_descriptors_.at(fd), size_error);
        if (size_error || !memory_.write64(registers[2], size / 512U)) {
          bsd_error(cpu, size_error ? 5U : bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      }
      case 0x40046419U: { // DKIOCGETBLOCKCOUNT32
        std::error_code size_error;
        const auto size =
            std::filesystem::file_size(file_descriptors_.at(fd), size_error);
        if (size_error ||
            !memory_.write32(registers[2],
                             static_cast<std::uint32_t>(size / 512U))) {
          bsd_error(cpu, size_error ? 5U : bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      }
      case 0x40046417U: // DKIOCISFORMATTED
      case 0x4004641dU: // DKIOCISWRITABLE
        if (!memory_.write32(registers[2], 1)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      case 0x20006416U: // DKIOCSYNCHRONIZECACHE
        bsd_success(cpu, 0);
        return;
      default: {
        std::ostringstream message;
        message << "[disk] unsupported ioctl 0x" << std::hex << registers[1]
                << " minor=" << std::dec << block->second.first << '\n';
        output_.write(message.str());
        bsd_error(cpu, 25);
        return;
      }
      }
    }
    const auto device = virtual_descriptors_.find(fd);
    if (device == virtual_descriptors_.end()) {
      bsd_error(cpu, 25); // ENOTTY
      return;
    }
    if (registers[1] == darwin::socket::ioctl_non_block) {
      const auto enabled = memory_.read32(registers[2]);
      if (!enabled) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      auto &flags = file_status_flags_[fd];
      if (*enabled != 0)
        flags |= darwin::open_flag::non_block;
      else
        flags &= ~darwin::open_flag::non_block;
      bsd_success(cpu, 0);
      return;
    }
    if (ioctl_bpf_device(cpu, fd))
      return;
    if (ioctl_kernel_control_socket(cpu))
      return;
    const auto tty_descriptor =
        device->second == "console" ||
        device->second == bsd::baseband_device::descriptor_kind ||
        device->second == bsd::offline_serial_device::descriptor_kind;
    if (tty_descriptor &&
        registers[1] == darwin::tty::set_controlling_terminal) {
      output_.write("[tty] controlling-terminal pid=" +
                    std::to_string(process_.pid) + " fd=" +
                    std::to_string(fd) + "\n");
      bsd_success(cpu, 0);
      return;
    }
    if (tty_descriptor &&
        registers[1] == darwin::tty::set_arbitrary_speed) {
      const auto speed = memory_.read32(registers[2]);
      if (!speed) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (device->second == bsd::baseband_device::descriptor_kind) {
        auto attributes = shared_state_->baseband_device_state.attributes();
        attributes.input_speed = static_cast<std::int32_t>(*speed);
        attributes.output_speed = static_cast<std::int32_t>(*speed);
        shared_state_->baseband_device_state.set_attributes(attributes);
      } else {
        auto attributes = offline_serial_state_.attributes();
        attributes.input_speed = static_cast<std::int32_t>(*speed);
        attributes.output_speed = static_cast<std::int32_t>(*speed);
        offline_serial_state_.set_attributes(attributes);
      }
      bsd_success(cpu, 0);
      return;
    }
    if (tty_descriptor && registers[1] == darwin::tty::drain_output) {
      // Both virtual serial implementations consume output synchronously.
      bsd_success(cpu, 0);
      return;
    }
    if (device->second == bsd::offline_serial_device::descriptor_kind) {
      const auto request = registers[1];
      const auto argument = registers[2];
      if (request == darwin::tty::get_attributes) {
        const auto attributes = offline_serial_state_.attributes();
        const auto control_characters =
            std::as_bytes(std::span{attributes.control_characters});
        if (!memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::input_flags,
                attributes.input_flags) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::output_flags,
                attributes.output_flags) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::control_flags,
                attributes.control_flags) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::local_flags,
                attributes.local_flags) ||
            !memory_.copy_in(
                argument +
                    darwin::tty::arm32_attributes_offset::control_characters,
                control_characters) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::input_speed,
                static_cast<std::uint32_t>(attributes.input_speed)) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::output_speed,
                static_cast<std::uint32_t>(attributes.output_speed))) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        bsd_success(cpu, 0);
        return;
      }
      if (request == darwin::tty::set_attributes ||
          request == darwin::tty::set_attributes_after_drain ||
          request == darwin::tty::set_attributes_after_drain_and_flush) {
        const auto input_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::input_flags);
        const auto output_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::output_flags);
        const auto control_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::control_flags);
        const auto local_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::local_flags);
        const auto control_characters = memory_.read_bytes(
            argument + darwin::tty::arm32_attributes_offset::control_characters,
            darwin::tty::control_character_count);
        const auto input_speed = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::input_speed);
        const auto output_speed = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::output_speed);
        if (!input_flags || !output_flags || !control_flags || !local_flags ||
            !control_characters || !input_speed || !output_speed) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        darwin::tty::Arm32Attributes attributes{
            .input_flags = *input_flags,
            .output_flags = *output_flags,
            .control_flags = *control_flags,
            .local_flags = *local_flags,
            .input_speed = static_cast<std::int32_t>(*input_speed),
            .output_speed = static_cast<std::int32_t>(*output_speed),
        };
        std::transform(
            control_characters->begin(), control_characters->end(),
            attributes.control_characters.begin(),
            [](std::byte value) {
              return std::to_integer<std::uint8_t>(value);
            });
        offline_serial_state_.set_attributes(attributes);
        bsd_success(cpu, 0);
        return;
      }
      if (request == darwin::tty::set_exclusive ||
          request == darwin::tty::clear_exclusive) {
        bsd_success(cpu, 0);
        return;
      }
      std::ostringstream message;
      message << "[serial] unsupported offline ioctl pid=" << process_.pid
              << " request=0x" << std::hex << request << '\n';
      output_.write(message.str());
      bsd_error(cpu, darwin::error::inappropriate_ioctl);
      return;
    }
    if (device->second == bsd::baseband_device::descriptor_kind) {
      const auto request = registers[1];
      const auto argument = registers[2];
      if (request == darwin::tty::get_attributes) {
        const auto attributes =
            shared_state_->baseband_device_state.attributes();
        const auto control_characters =
            std::as_bytes(std::span{attributes.control_characters});
        if (!memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::input_flags,
                attributes.input_flags) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::output_flags,
                attributes.output_flags) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::control_flags,
                attributes.control_flags) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::local_flags,
                attributes.local_flags) ||
            !memory_.copy_in(
                argument +
                    darwin::tty::arm32_attributes_offset::control_characters,
                control_characters) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::input_speed,
                static_cast<std::uint32_t>(attributes.input_speed)) ||
            !memory_.write32(
                argument + darwin::tty::arm32_attributes_offset::output_speed,
                static_cast<std::uint32_t>(attributes.output_speed))) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        output_.write("[baseband] ioctl pid=" + std::to_string(process_.pid) +
                      " TIOCGETA\n");
        bsd_success(cpu, 0);
        return;
      }
      if (request == darwin::tty::set_attributes ||
          request == darwin::tty::set_attributes_after_drain ||
          request == darwin::tty::set_attributes_after_drain_and_flush) {
        const auto input_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::input_flags);
        const auto output_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::output_flags);
        const auto control_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::control_flags);
        const auto local_flags = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::local_flags);
        const auto control_characters = memory_.read_bytes(
            argument + darwin::tty::arm32_attributes_offset::control_characters,
            darwin::tty::control_character_count);
        const auto input_speed = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::input_speed);
        const auto output_speed = memory_.read32(
            argument + darwin::tty::arm32_attributes_offset::output_speed);
        if (!input_flags || !output_flags || !control_flags || !local_flags ||
            !control_characters || !input_speed || !output_speed) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        darwin::tty::Arm32Attributes attributes{
            .input_flags = *input_flags,
            .output_flags = *output_flags,
            .control_flags = *control_flags,
            .local_flags = *local_flags,
            .input_speed = static_cast<std::int32_t>(*input_speed),
            .output_speed = static_cast<std::int32_t>(*output_speed),
        };
        std::transform(control_characters->begin(), control_characters->end(),
                       attributes.control_characters.begin(),
                       [](std::byte value) {
                         return std::to_integer<std::uint8_t>(value);
                       });
        shared_state_->baseband_device_state.set_attributes(attributes);
        std::ostringstream message;
        message << "[baseband] ioctl pid=" << process_.pid
                << " TIOCSETA request=0x" << std::hex << request << std::dec
                << " ispeed=" << attributes.input_speed
                << " ospeed=" << attributes.output_speed << '\n';
        output_.write(message.str());
        bsd_success(cpu, 0);
        return;
      }
      if (request == darwin::tty::set_h5_transport_mode) {
        const auto enabled = memory_.read32(argument);
        if (!enabled) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        shared_state_->baseband_device_state.set_h5_transport_mode(
            *enabled != 0);
        output_.write("[baseband] ioctl pid=" +
                      std::to_string(process_.pid) +
                      " IOAOSH5 enabled=" +
                      std::to_string(*enabled != 0) + "\n");
        bsd_success(cpu, 0);
        return;
      }
    if (request == darwin::tty::set_receive_threshold) {
      const auto threshold = memory_.read32(argument);
      if (!threshold) {
        bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        shared_state_->baseband_device_state.set_minimum_receive_bytes(
            *threshold);
        if (!memory_.write32(argument, *threshold)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        output_.write("[baseband] ioctl pid=" +
                      std::to_string(process_.pid) +
                      " IOAOSMIN minimum=" + std::to_string(*threshold) +
                      "\n");
        bsd_success(cpu, 0);
        return;
      }
      if (request == darwin::tty::asm_new_dlci) {
        const auto payload_size = darwin::tty::parameter_length(request);
        const auto payload = memory_.read_bytes(argument, payload_size);
        if (!payload) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        const auto channel_name =
            extract_mux_channel_name(memory_, argument, *payload);
        const auto channel_unit = shared_state_->baseband_device_state
                                       .register_mux_channel(channel_name
                                                                 ? *channel_name
                                                                 : std::string{});
        if (!memory_.copy_in(argument, *payload) ||
            !memory_.write32(argument, 0U) ||
            !memory_.write32(argument + sizeof(std::uint32_t),
                             channel_unit)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        std::ostringstream message;
        message << "[baseband] ioctl pid=" << process_.pid
                << " ASMIOCNEWDLCI unit=" << channel_unit;
        if (channel_name) {
          message << " name=" << *channel_name;
        }
        message << '\n';
        output_.write(message.str());
        bsd_success(cpu, 0);
        return;
      }
      if (shared_state_->baseband_device_state.ioctl(registers[1]) ==
          bsd::baseband_device::IoctlResult::success) {
        std::ostringstream message;
        message << "[baseband] ioctl pid=" << process_.pid << " request=0x"
                << std::hex << registers[1] << std::dec << " exclusive="
                << shared_state_->baseband_device_state.exclusive() << '\n';
        output_.write(message.str());
        bsd_success(cpu, 0);
        return;
      }
      std::ostringstream message;
      message << "[baseband] passthrough ioctl pid=" << process_.pid
              << " request=0x" << std::hex << registers[1];
      constexpr std::uint32_t maximum_traced_ioctl_payload = 64;
      const auto payload_size = darwin::tty::parameter_length(registers[1]);
      if ((registers[1] & darwin::tty::ioctl_input) != 0 && payload_size != 0 &&
          payload_size <= maximum_traced_ioctl_payload) {
        if (const auto payload =
                memory_.read_bytes(registers[2], payload_size)) {
          message << " input=" << bsd_support::format_payload_prefix(*payload);
        }
      }
      message << '\n';
      output_.write(message.str());
      bsd_success(cpu, 0);
      return;
    }
    if (registers[1] == darwin::socket::ioctl_pending_bytes) {
      std::uint32_t pending_error = 0;
      const auto pending_count = socket_pending_byte_count(fd, pending_error);
      if (!pending_count) {
        bsd_error(cpu, pending_error);
        return;
      }
      if (!memory_.write32(registers[2], *pending_count)) {
        bsd_error(cpu, bsd_support::bad_address);
      } else {
        bsd_success(cpu, 0);
      }
      return;
    }
    if (device->second == "inet-dgram" || device->second == "inet6-dgram") {
      const auto name_bytes = memory_.read_bytes(registers[2], 16);
      if (!name_bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      std::string name;
      for (const auto byte : *name_bytes) {
        const auto value = static_cast<char>(byte);
        if (value == '\0')
          break;
        name.push_back(value);
      }
      namespace wifi_driver = darwin::network::apple80211_driver;
      if (registers[1] == wifi_driver::set_request ||
          registers[1] == wifi_driver::get_request) {
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
          std::lock_guard mach_lock{shared_state_->mach_mutex};
          service_available = shared_state_->wifi_service_available;
        }
        if (name != "en0" || !service_available) {
          output_.write("[wifi-driver] reject pid=" +
                        std::to_string(process_.pid) + " fd=" +
                        std::to_string(fd) + " interface=" + name + "\n");
          bsd_error(cpu, 6); // ENXIO
          return;
        }
        if (registers[1] == wifi_driver::set_request &&
            *command == wifi_driver::command_scan) {
          apple80211_scan_delivered_.erase(fd);
          static_cast<void>(wifi_state_->scan());
          output_.write("[wifi-driver] scan-start pid=" +
                        std::to_string(process_.pid) + " fd=" +
                        std::to_string(fd) + "\n");
          bsd_success(cpu, 0);
          return;
        }
        if (registers[1] == wifi_driver::set_request &&
            *command == wifi_driver::command_power) {
          if (*data_address == 0 ||
              *data_length < wifi_driver::power_state_size) {
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
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
          output_.write("[wifi-driver] probe pid=" +
                        std::to_string(process_.pid) + " fd=" +
                        std::to_string(fd) + " interface=en0\n");
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
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
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
                  wifi_state_->snapshot().associated_access_point ? 1U
                                                                  : 0U)) {
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
          const auto associated =
              wifi_state_->snapshot().associated_access_point;
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
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
                               : bsd_support::invalid_argument);
            return;
          }
          const auto associated =
              wifi_state_->snapshot().associated_access_point;
          if (!associated) {
            // A successful command means that an association exists. Returning
            // an empty successful payload makes legacy Apple80211 clients
            // construct a non-null "current network" object from a scan result.
            bsd_error(cpu, 57); // ENOTCONN
            return;
          }
          const auto copied = std::min<std::size_t>(
              associated->ssid.size(), wifi_driver::current_ssid_size);
          std::array<std::byte, wifi_driver::current_ssid_size> ssid{};
          std::transform(
              associated->ssid.begin(), associated->ssid.begin() + copied,
              ssid.begin(), [](char value) {
                return static_cast<std::byte>(
                    static_cast<unsigned char>(value));
              });
          if (!memory_.copy_in(
                  *data_address,
                  std::span{ssid.data(), copied}) ||
              !memory_.write32(
                  registers[2] + wifi_driver::data_length_offset,
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
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
                               : bsd_support::invalid_argument);
            return;
          }
          const auto associated =
              wifi_state_->snapshot().associated_access_point;
          if (!associated) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
          }
          if (!memory_.copy_in(*data_address, std::span{
                  associated->bssid.data(), associated->bssid.size()})) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
          }
          bsd_success(cpu, 0);
          return;
        }
        if (registers[1] == wifi_driver::get_request &&
            *command == wifi_driver::command_channel) {
          if (*data_address == 0 ||
              *data_length < wifi_driver::channel_state_size) {
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
                               : bsd_support::invalid_argument);
            return;
          }
          const auto associated =
              wifi_state_->snapshot().associated_access_point;
          if (!associated) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
          }
          if (!memory_.write32(
                  *data_address + wifi_driver::channel_state_channel_offset,
                  associated->channel) ||
              !memory_.write32(
                  *data_address + wifi_driver::channel_state_flags_offset,
                  0)) {
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
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
                               : bsd_support::invalid_argument);
            return;
          }
          const auto associated =
              wifi_state_->snapshot().associated_access_point;
          if (!associated) {
            bsd_error(cpu, 57); // ENOTCONN
            return;
          }
          const auto signal = static_cast<std::uint32_t>(
              *command == wifi_driver::command_rssi
                  ? associated->rssi
                  : -90);
          if (!memory_.write32(
                  *data_address + wifi_driver::signal_state_count_offset,
                  1) ||
              !memory_.write32(
                  *data_address + wifi_driver::signal_state_unit_offset,
                  0) ||
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
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
                               : bsd_support::invalid_argument);
            return;
          }
          constexpr auto driver_name = wifi_driver::event_device_name;
          std::vector<std::byte> name_buffer(
              std::min<std::size_t>(*data_length, driver_name.size() + 1),
              std::byte{0});
          std::transform(
              driver_name.begin(),
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
            output_.write("[wifi-driver] scan-end pid=" +
                          std::to_string(process_.pid) + " fd=" +
                          std::to_string(fd) + "\n");
            bsd_error(cpu, 5); // EIO terminates the firmware scan iterator.
            return;
          }
          const auto access_points = wifi_state_->scan();
          if (access_points.empty()) {
            bsd_error(cpu, 5);
            return;
          }
          const auto &access_point = access_points.front();
          if (*data_address == 0 ||
              *data_length < wifi_driver::scan_result_size ||
              access_point.ssid.size() > 32) {
            bsd_error(cpu, *data_address == 0
                               ? bsd_support::bad_address
                               : bsd_support::invalid_argument);
            return;
          }
          const auto ie_scratch = memory_.read32(
              *data_address + wifi_driver::scan_ie_pointer_offset);
          if (!ie_scratch) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
          }
          std::vector<std::byte> result(
              wifi_driver::scan_result_size, std::byte{0});
          const auto write16 = [&result](std::uint32_t offset,
                                         std::uint16_t value) {
            result[offset] = static_cast<std::byte>(value & 0xffU);
            result[offset + 1] =
                static_cast<std::byte>((value >> 8U) & 0xffU);
          };
          const auto write32 = [&result](std::uint32_t offset,
                                         std::uint32_t value) {
            for (std::uint32_t index = 0; index < 4; ++index) {
              result[offset + index] =
                  static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
            }
          };
          // Preserve only the firmware-owned IE scratch pointer. Copying the
          // entire input record would leak uninitialized caller flags into
          // the native parser and can make a discovered AP appear associated.
          write32(wifi_driver::scan_ie_pointer_offset, *ie_scratch);
          write32(wifi_driver::scan_channel_offset, access_point.channel);
          write32(wifi_driver::scan_channel_flags_offset, 0);
          write16(wifi_driver::scan_noise_offset,
                  static_cast<std::uint16_t>(
                      static_cast<std::int16_t>(-90)));
          write16(wifi_driver::scan_rssi_offset,
                  static_cast<std::uint16_t>(
                      static_cast<std::int16_t>(access_point.rssi)));
          write16(wifi_driver::scan_beacon_interval_offset, 100);
          // IEEE 802.11 capability bit 0 is ESS. Privacy (bit 4) is set only
          // for networks whose association requires a key.
          write16(
              wifi_driver::scan_capabilities_offset,
              static_cast<std::uint16_t>(
                  1U |
                  (access_point.security == WifiSecurity::Open ? 0U : 0x10U)));
          std::copy(access_point.bssid.begin(), access_point.bssid.end(),
                    result.begin() + wifi_driver::scan_bssid_offset);
          result[wifi_driver::scan_rate_count_offset] = std::byte{0};
          result[wifi_driver::scan_ssid_length_offset] =
              static_cast<std::byte>(access_point.ssid.size());
          std::fill_n(result.begin() + wifi_driver::scan_ssid_offset, 32,
                      std::byte{0});
          std::transform(
              access_point.ssid.begin(), access_point.ssid.end(),
              result.begin() + wifi_driver::scan_ssid_offset,
              [](char value) {
                return static_cast<std::byte>(
                    static_cast<unsigned char>(value));
              });
          write32(wifi_driver::scan_age_offset, 0);
          write16(wifi_driver::scan_ie_length_offset, 0);
          if (!memory_.copy_in(*data_address, result)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
          }
          apple80211_scan_delivered_.insert(fd);
          output_.write("[wifi-driver] scan-result pid=" +
                        std::to_string(process_.pid) + " fd=" +
                        std::to_string(fd) + " ssid=" +
                        access_point.ssid + "\n");
          bsd_success(cpu, 0);
          return;
        }
        output_.write("[wifi-driver] unsupported pid=" +
                      std::to_string(process_.pid) + " fd=" +
                      std::to_string(fd) + " operation=" +
                      (registers[1] == wifi_driver::get_request ? "get"
                                                               : "set") +
                      " command=" + std::to_string(*command) +
                      " length=" + std::to_string(*data_length) + "\n");
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      std::unique_lock network_lock{shared_state_->network_mutex};
      const auto interface = shared_state_->network_interfaces.find(name);
      if (interface == shared_state_->network_interfaces.end()) {
        bsd_error(cpu, 6); // ENXIO
        return;
      }
      const auto ioctl_identity = darwin::network::ioctl_identity(registers[1]);
      if (ioctl_identity == darwin::network::ioctl_get_interface_media) {
        const auto ethernet =
            interface->second.type == darwin::network::interface_type_ethernet;
        const auto active = (interface->second.flags &
                             (darwin::network::interface_flag_up |
                              darwin::network::interface_flag_running)) ==
                            (darwin::network::interface_flag_up |
                             darwin::network::interface_flag_running);
        const auto automatic_media = darwin::network::media_type_ethernet |
                                     darwin::network::media_subtype_auto;
        const auto active_media = darwin::network::media_type_ethernet |
                                  darwin::network::media_subtype_100_tx |
                                  darwin::network::media_option_full_duplex;
        const std::array<std::uint32_t, 2> available_media{automatic_media,
                                                           active_media};
        const auto requested_count = memory_.read32(
            registers[2] + darwin::network::interface_media_count_offset);
        const auto list_address = memory_.read32(
            registers[2] + darwin::network::interface_media_list_offset);
        if (!requested_count || !list_address) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        if (ethernet && *list_address != 0) {
          const auto copied =
              std::min<std::size_t>(*requested_count, available_media.size());
          for (std::size_t index = 0; index < copied; ++index) {
            if (!memory_.write32(*list_address +
                                     static_cast<std::uint32_t>(
                                         index * sizeof(std::uint32_t)),
                                 available_media[index])) {
              bsd_error(cpu, bsd_support::bad_address);
              return;
            }
          }
        }
        const auto media_status =
            ethernet ? darwin::network::media_status_valid |
                           (active ? darwin::network::media_status_active : 0U)
                     : 0U;
        if (!memory_.write32(
                registers[2] + darwin::network::interface_media_current_offset,
                ethernet ? active_media : 0U) ||
            !memory_.write32(registers[2] +
                                 darwin::network::interface_media_mask_offset,
                             0) ||
            !memory_.write32(registers[2] +
                                 darwin::network::interface_media_status_offset,
                             media_status) ||
            !memory_.write32(registers[2] +
                                 darwin::network::interface_media_active_offset,
                             ethernet ? active_media : 0U) ||
            !memory_.write32(
                registers[2] + darwin::network::interface_media_count_offset,
                ethernet ? static_cast<std::uint32_t>(available_media.size())
                         : 0U)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        bsd_success(cpu, 0);
        return;
      }
      if (ioctl_identity == darwin::network::ioctl_set_interface_media) {
        const auto media = memory_.read32(
            registers[2] + darwin::network::interface_request_value_offset);
        if (!media) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        if (interface->second.type !=
                darwin::network::interface_type_ethernet ||
            (*media & darwin::network::media_type_mask) !=
                darwin::network::media_type_ethernet) {
          bsd_error(cpu, bsd_support::invalid_argument);
          return;
        }
        bsd_success(cpu, 0);
        return;
      }
      if (ioctl_identity == darwin::network::ioctl_get_interface_mtu) {
        if (!memory_.write32(
                registers[2] + darwin::network::interface_request_value_offset,
                interface->second.mtu)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      }
      if (ioctl_identity == darwin::network::ioctl_set_interface_mtu) {
        const auto mtu = memory_.read32(
            registers[2] + darwin::network::interface_request_value_offset);
        if (!mtu) {
          bsd_error(cpu, bsd_support::bad_address);
        } else if (*mtu < 68U || *mtu > 65'535U) {
          bsd_error(cpu, bsd_support::invalid_argument);
        } else {
          interface->second.mtu = *mtu;
          bsd_success(cpu, 0);
        }
        return;
      }
      if (ioctl_identity == darwin::network::ioctl_get_ipv6_address_flags) {
        if (!memory_.write32(
                registers[2] + darwin::network::interface_request_value_offset,
                0)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      }
      if (ioctl_identity == 0x80006919U) { // SIOCDIFADDR[_IN6]
        const auto ipv6 = device->second == "inet6-dgram";
        const auto family = static_cast<std::uint8_t>(
            ipv6 ? darwin::network::address_family_inet6
                 : darwin::network::address_family_inet);
        const auto address_size = ipv6 ? 28U : 16U;
        const auto requested =
            memory_.read_bytes(registers[2] + 16, address_size);
        if (!requested) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        const auto configured =
            ipv6 ? interface->second.has_ipv6 : interface->second.has_ipv4;
        const auto address_matches =
            ipv6 ? std::equal(requested->begin(), requested->end(),
                              interface->second.ipv6_address.begin())
                 : std::equal(requested->begin(), requested->end(),
                              interface->second.ipv4_address.begin());
        if (!configured || !address_matches) {
          bsd_error(cpu, darwin::error::address_not_available);
          return;
        }
        const auto deleted_address_snapshot =
            kernel_network::make_interface_snapshot(name, interface->second);
        if (ipv6) {
          interface->second.has_ipv6 = false;
          interface->second.ipv6_address = {};
          interface->second.ipv6_netmask = {};
        } else {
          interface->second.has_ipv4 = false;
          interface->second.ipv4_address = {};
          interface->second.ipv4_netmask = {};
          interface->second.ipv4_broadcast = {};
          interface->second.ipv4_gateway = {};
        }
        network_lock.unlock();
        synchronize_interface_routes(name, family);
        post_network_event(
            name,
            ipv6 ? darwin::network::kernel_event_inet6_subclass
                 : darwin::network::kernel_event_inet_subclass,
            ipv6 ? darwin::network::kernel_event_inet6_address_deleted
                 : darwin::network::kernel_event_inet_address_deleted,
            deleted_address_snapshot);
        bsd_success(cpu, 0);
        return;
      }
      switch (registers[1]) {
      case 0xc0206911U: // SIOCGIFFLAGS
        if (!memory_.write16(registers[2] + 16, interface->second.flags)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        bsd_success(cpu, 0);
        return;
      case 0x80206910U: { // SIOCSIFFLAGS
        const auto flags = memory_.read16(registers[2] + 16);
        if (!flags) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        const auto kernel_managed =
            darwin::network::interface_flags_kernel_managed;
        interface->second.flags = static_cast<std::uint16_t>(
            (interface->second.flags & kernel_managed) |
            (*flags & static_cast<std::uint16_t>(~kernel_managed)));
        network_lock.unlock();
        post_data_link_event(
            name, darwin::network::kernel_event_interface_flags_changed);
        bsd_success(cpu, 0);
        return;
      }
      case 0x8040691aU: { // SIOCAIFADDR, struct ifaliasreq
        const auto previously_configured = interface->second.has_ipv4;
        const auto address = memory_.read_bytes(registers[2] + 16, 16);
        const auto broadcast = memory_.read_bytes(registers[2] + 32, 16);
        const auto netmask = memory_.read_bytes(registers[2] + 48, 16);
        if (!address || !broadcast || !netmask) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        if (std::to_integer<std::uint8_t>((*address)[0]) != 16 ||
            std::to_integer<std::uint8_t>((*address)[1]) !=
                darwin::network::address_family_inet) {
          bsd_error(cpu, bsd_support::invalid_argument);
          return;
        }
        std::copy(address->begin(), address->end(),
                  interface->second.ipv4_address.begin());
        std::copy(netmask->begin(), netmask->end(),
                  interface->second.ipv4_netmask.begin());
        std::copy(broadcast->begin(), broadcast->end(),
                  interface->second.ipv4_broadcast.begin());
        interface->second.has_ipv4 = true;
        network_lock.unlock();
        synchronize_interface_routes(name,
                                     darwin::network::address_family_inet);
        post_network_event(
            name, darwin::network::kernel_event_inet_subclass,
            previously_configured
                ? darwin::network::kernel_event_inet_address_changed
                : darwin::network::kernel_event_inet_new_address);
        bsd_success(cpu, 0);
        return;
      }
      case 0x8078691aU: { // SIOCAIFADDR_IN6, struct in6_aliasreq
        const auto address = memory_.read_bytes(registers[2] + 16, 28);
        const auto netmask = memory_.read_bytes(registers[2] + 72, 28);
        if (!address || !netmask) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        if (std::to_integer<std::uint8_t>((*address)[0]) != 28 ||
            std::to_integer<std::uint8_t>((*address)[1]) !=
                darwin::network::address_family_inet6) {
          bsd_error(cpu, bsd_support::invalid_argument);
          return;
        }
        std::copy(address->begin(), address->end(),
                  interface->second.ipv6_address.begin());
        std::copy(netmask->begin(), netmask->end(),
                  interface->second.ipv6_netmask.begin());
        interface->second.has_ipv6 = true;
        network_lock.unlock();
        synchronize_interface_routes(name,
                                     darwin::network::address_family_inet6);
        post_network_event(
            name, darwin::network::kernel_event_inet6_subclass,
            darwin::network::kernel_event_inet6_new_user_address);
        bsd_success(cpu, 0);
        return;
      }
      default: {
        std::ostringstream message;
        message << "[network] unsupported ioctl 0x" << std::hex << registers[1]
                << " on " << name << '\n';
        output_.write(message.str());
        bsd_error(cpu, 25);
        return;
      }
      }
    }
    if (device->second != "system-event-socket") {
      bsd_error(cpu, 25);
      return;
    }
    if (registers[1] == 0x800c6502U) { // SIOCSKEVFILT
      std::array<std::uint32_t, 3> filter{};
      for (std::size_t index = 0; index < filter.size(); ++index) {
        const auto value = memory_.read32(
            registers[2] + static_cast<std::uint32_t>(index * 4U));
        if (!value) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        filter[index] = *value;
      }
      system_event_filters_[fd] = filter;
      bsd_success(cpu, 0);
      return;
    }
    if (registers[1] == 0x400c6503U) { // SIOCGKEVFILT
      const auto filter = system_event_filters_[fd];
      for (std::size_t index = 0; index < filter.size(); ++index) {
        if (!memory_.write32(registers[2] +
                                 static_cast<std::uint32_t>(index * 4U),
                             filter[index])) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if (registers[1] == 0x40046501U) { // SIOCGKEVID
      std::uint32_t last_identifier = 0;
      {
        std::lock_guard socket_lock{shared_state_->socket_mutex};
        last_identifier = shared_state_->next_kernel_event_identifier - 1U;
      }
      if (!memory_.write32(registers[2], last_identifier)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      bsd_success(cpu, 0);
      return;
    }
    bsd_error(cpu, 25); // ENOTTY
    return;
  }
  case 93: { // select
    constexpr std::uint64_t microseconds_per_second = 1'000'000ULL;
    constexpr std::uint64_t nanoseconds_per_microsecond = 1'000ULL;
    const auto descriptor_count = registers[0];
    if (descriptor_count > 1024) {
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
    const auto words = (descriptor_count + 31U) / 32U;
    std::uint32_t ready_count = 0;
    std::vector<std::uint32_t> requested_read_words(words);
    std::vector<std::uint32_t> requested_write_words(words);
    for (std::uint32_t word_index = 0; word_index < words; ++word_index) {
      std::uint32_t ready_read_word = 0;
      std::uint32_t ready_write_word = 0;
      if (registers[1] != 0) {
        const auto requested = memory_.read32(registers[1] + word_index * 4U);
        if (!requested) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        requested_read_words[word_index] = *requested;
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
          const auto fd = word_index * 32U + bit;
          if (fd >= descriptor_count || (*requested & (1U << bit)) == 0)
            continue;
          if (descriptor_readable(fd)) {
            ready_read_word |= 1U << bit;
            ++ready_count;
          }
        }
        if (!memory_.write32(registers[1] + word_index * 4U, ready_read_word)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      if (registers[2] != 0) {
        const auto requested = memory_.read32(registers[2] + word_index * 4U);
        if (!requested) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        requested_write_words[word_index] = *requested;
        for (std::uint32_t bit = 0; bit < 32; ++bit) {
          const auto fd = word_index * 32U + bit;
          if (fd >= descriptor_count || (*requested & (1U << bit)) == 0) {
            continue;
          }
          if (descriptor_writable(fd)) {
            ready_write_word |= 1U << bit;
            ++ready_count;
          }
        }
        if (!memory_.write32(registers[2] + word_index * 4U,
                             ready_write_word)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      if (registers[3] != 0 &&
          !memory_.write32(registers[3] + word_index * 4U, 0)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
    }
    if (ready_count != 0) {
      bsd_success(cpu, ready_count);
      return;
    }
    std::optional<std::uint64_t> timeout_deadline;
    if (registers[4] != 0) {
      const auto seconds_word = memory_.read32(registers[4]);
      const auto microseconds_word = memory_.read32(registers[4] + 4);
      if (!seconds_word || !microseconds_word) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      const auto seconds = static_cast<std::int32_t>(*seconds_word);
      const auto microseconds = static_cast<std::int32_t>(*microseconds_word);
      if (seconds < 0 || microseconds < 0 ||
          static_cast<std::uint64_t>(microseconds) >=
              microseconds_per_second) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto duration =
          (static_cast<std::uint64_t>(seconds) * microseconds_per_second +
           static_cast<std::uint64_t>(microseconds)) *
          nanoseconds_per_microsecond;
      if (duration == 0) {
        bsd_success(cpu, 0);
        return;
      }
      // select(2)'s timeout applies to every descriptor provider, including a
      // disconnected character device with no host poll source.
      const auto now = shared_state_->clock.now();
      timeout_deadline =
          duration > std::numeric_limits<std::uint64_t>::max() - now
              ? std::numeric_limits<std::uint64_t>::max()
              : now + duration;
    }
    process_.waiting_for_events = true;
    pending_selects_[cpu.processor_id()] =
        PendingSelect{descriptor_count,
                      registers[1],
                      registers[2],
                      registers[3],
                      std::move(requested_read_words),
                      std::move(requested_write_words),
                      cpu.processor_id(),
                      timeout_deadline};
    output_.write("[network] select wait pid=" + std::to_string(process_.pid) +
                  " nfds=" + std::to_string(descriptor_count) + "\n");
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
    return;
  }
  case 202: { // __sysctl
    const auto mib0 = memory_.read32(registers[0]);
    const auto mib1 =
        registers[1] >= 2 ? memory_.read32(registers[0] + 4) : std::nullopt;
    const auto old_size = registers[3] != 0 ? memory_.read32(registers[3])
                                            : std::optional<std::uint32_t>{0};
    if (!mib0 || !mib1 || !old_size) {
      bsd_error(cpu, bsd_support::bad_address);
      return;
    }
    if (*mib0 == darwin::sysctl::control_unspecified &&
        *mib1 == darwin::sysctl::operation_name_to_oid && registers[1] == 2) {
      // XNU's sysctl.name2oid consumes an un-terminated name through the new
      // value and returns the integer MIB through the old value.
      constexpr std::uint32_t maximum_name_length = 1024;
      if (registers[4] == 0 || registers[5] == 0 ||
          registers[5] >= maximum_name_length) {
        bsd_error(cpu, registers[5] >= maximum_name_length
                           ? darwin::error::argument_list_too_long
                           : darwin::error::no_entry);
        return;
      }
      const auto name_bytes = memory_.read_bytes(registers[4], registers[5]);
      if (!name_bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      std::string_view name{reinterpret_cast<const char *>(name_bytes->data()),
                            name_bytes->size()};
      if (const auto terminator = name.find('\0');
          terminator != std::string_view::npos) {
        name = name.substr(0, terminator);
      }
      const auto identifier = darwin::sysctl::resolve_name(name);
      if (!identifier) {
        bsd_error(cpu, darwin::error::no_entry);
        return;
      }
      const auto required =
          static_cast<std::uint32_t>(identifier->size * sizeof(std::uint32_t));
      if (registers[3] == 0 || !memory_.write32(registers[3], required)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[2] != 0) {
        if (*old_size < required) {
          bsd_error(cpu, darwin::error::no_memory);
          return;
        }
        for (std::size_t index = 0; index < identifier->size; ++index) {
          if (!memory_.write32(registers[2] +
                                   static_cast<std::uint32_t>(index * 4U),
                               identifier->components[index])) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
          }
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    const auto route_operation =
        registers[1] == 6 ? memory_.read32(registers[0] + 16) : std::nullopt;
    if (*mib0 == 4 && *mib1 == darwin::route::protocol_family &&
        route_operation &&
        (*route_operation == darwin::route::sysctl_dump ||
         *route_operation == darwin::route::sysctl_flags ||
         *route_operation == darwin::route::sysctl_dump2)) {
      // CTL_NET/PF_ROUTE/NET_RT_DUMP, NET_RT_FLAGS, or NET_RT_DUMP2.
      // rt_msghdr and ARM32 rt_msghdr2 are both 92 bytes; offsets 16,
      // 20, and 24 become refcnt/parentflags/reserved for RTM_GET2.
      const auto address_family = memory_.read32(registers[0] + 12);
      const auto flags = memory_.read32(registers[0] + 20);
      if (!address_family || !flags || registers[3] == 0) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[4] != 0) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
      }
      const auto routes = shared_state_->route_table.snapshot();
      const auto records = darwin::route::make_table_dump(
          routes, *address_family,
          *route_operation == darwin::route::sysctl_flags ? *flags : 0U,
          *route_operation == darwin::route::sysctl_dump2
              ? darwin::route::message_get2
              : darwin::route::message_get);
      const auto required = static_cast<std::uint32_t>(records.size());
      if (!memory_.write32(registers[3], required)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[2] != 0) {
        if (*old_size < required) {
          bsd_error(cpu, darwin::error::no_memory);
          return;
        }
        if (!memory_.copy_in(registers[2], records)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == 4 && *mib1 == darwin::route::protocol_family &&
        registers[1] == 6 &&
        memory_.read32(registers[0] + 16).value_or(0) ==
            darwin::route::sysctl_interface_list) {
      // CTL_NET/PF_ROUTE/NET_RT_IFLIST. XNU returns a sequence of
      // variable-length if_msghdr/ifa_msghdr records, not host ifaddrs.
      const auto address_family = memory_.read32(registers[0] + 12);
      const auto interface_index = memory_.read32(registers[0] + 20);
      if (!address_family || !interface_index || registers[3] == 0) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[4] != 0) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
      }
      std::vector<darwin::network::InterfaceSnapshot> interfaces;
      {
        std::lock_guard network_lock{shared_state_->network_mutex};
        interfaces.reserve(shared_state_->network_interfaces.size());
        for (const auto &[name, interface] :
             shared_state_->network_interfaces) {
          interfaces.push_back(
              kernel_network::make_interface_snapshot(name, interface));
        }
      }
      std::sort(interfaces.begin(), interfaces.end(),
                [](const auto &left, const auto &right) {
                  return left.index < right.index;
                });
      const auto records = darwin::network::make_route_interface_list(
          interfaces, *address_family, *interface_index);
      const auto required = static_cast<std::uint32_t>(records.size());
      if (!memory_.write32(registers[3], required)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[2] != 0) {
        if (*old_size < required) {
          bsd_error(cpu, 12); // ENOMEM
          return;
        }
        if (!memory_.copy_in(registers[2], records)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == darwin::sysctl::control_kernel &&
        (*mib1 == darwin::sysctl::kernel_process_arguments ||
         *mib1 == darwin::sysctl::kernel_process_arguments2) &&
        registers[1] == 3) { // KERN_PROCARGS[2] / pid
      const auto requested_pid = memory_.read32(registers[0] + 8U);
      if (!requested_pid || registers[3] == 0) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[4] != 0) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
      }
      const auto process = shared_state_->processes.find(*requested_pid);
      if (process == shared_state_->processes.end()) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto &record = process->second;
      const auto path = record.executable_path.empty()
                            ? std::string{"/"} + record.command
                            : record.executable_path;
      const auto bytes =
          *mib1 == darwin::sysctl::kernel_process_arguments2
              ? darwin::sysctl::encode_process_arguments2(
                    path, record.arguments, record.environment)
              : darwin::sysctl::encode_process_arguments(
                    path, record.arguments, record.environment);
      const auto required = static_cast<std::uint32_t>(bytes.size());
      if (registers[2] == 0) {
        if (!memory_.write32(registers[3], required)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      }
      const auto copied = std::min(*old_size, required);
      if (copied == 0 ||
          !memory_.copy_in(registers[2],
                           std::span<const std::byte>{bytes}.first(copied)) ||
          !memory_.write32(registers[3], copied)) {
        bsd_error(cpu, copied == 0 ? bsd_support::invalid_argument
                                   : bsd_support::bad_address);
        return;
      }
      bsd_success(cpu, 0);
      return;
    }
    const auto encode_process_info =
        [this](std::uint32_t pid,
               const KernelSharedState::ProcessRecord &record) {
          // XNU 792's 32-bit kinfo_proc is 492 bytes
          // (extern_proc=196, eproc=296).
          std::vector<std::byte> bytes(
              darwin::sysctl::arm32_kernel_process_info_size);
          const auto put16 = [&](std::size_t offset, std::uint16_t value) {
            bytes[offset] = static_cast<std::byte>(value);
            bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
          };
          const auto put32 = [&](std::size_t offset, std::uint32_t value) {
            for (std::size_t byte = 0; byte < 4; ++byte) {
              bytes[offset + byte] =
                  static_cast<std::byte>(value >> (byte * 8U));
            }
          };
          bytes[20] =
              static_cast<std::byte>(record.exited ? 5U : 2U); // SZOMB/SRUN
          put32(24, pid);
          if (const auto state =
                  shared_state_->process_kevent_states.find(pid);
              state != shared_state_->process_kevent_states.end() &&
              state->second.exec_generation != 0U) {
            // extern_proc.p_flag. P_EXEC is the level state paired with the
            // edge-triggered NOTE_EXEC notification: clients attach their
            // knote first, then inspect this bit to close the exec race.
            put32(16, darwin::sysctl::process_flag_exec);
          }
          bytes[162] = static_cast<std::byte>(
              pid == process_.pid ? process_.nice_value : 0);
          for (std::size_t index = 0;
               index < std::min<std::size_t>(16, record.command.size());
               ++index) {
            bytes[163 + index] =
                static_cast<std::byte>(record.command[index]);
          }
          put16(188, static_cast<std::uint16_t>(record.exit_status));
          put32(280, record.uid); // e_pcred.p_ruid
          put32(284, record.uid); // e_pcred.p_svuid
          put32(288, record.gid); // e_pcred.p_rgid
          put32(292, record.gid); // e_pcred.p_svgid
          put32(304, record.effective_uid); // e_ucred.cr_uid
          put16(308, 1);          // e_ucred.cr_ngroups
          put32(312, record.effective_gid); // e_ucred.cr_groups[0]
          put32(416, record.parent_pid);
          put32(420, record.process_group);
          return bytes;
        };
    const auto process_selector =
        *mib0 == darwin::sysctl::control_kernel &&
                *mib1 == darwin::sysctl::kernel_process &&
                registers[1] == 4
            ? memory_.read32(registers[0] + 8)
            : std::nullopt;
    if (process_selector &&
        *process_selector == darwin::sysctl::kernel_process_all) {
      if (registers[3] == 0) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[4] != 0) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
      }
      const auto record_size =
          darwin::sysctl::arm32_kernel_process_info_size;
      if (registers[2] == 0) {
        const auto estimated =
            static_cast<std::uint32_t>(shared_state_->processes.size() + 5U) *
            record_size;
        if (!memory_.write32(registers[3], estimated)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      }
      const auto required =
          static_cast<std::uint32_t>(shared_state_->processes.size()) *
          record_size;
      if (*old_size < required) {
        if (!memory_.write32(registers[3], required)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_error(cpu, darwin::error::no_memory);
        }
        return;
      }
      auto destination = registers[2];
      for (const auto &[pid, record] : shared_state_->processes) {
        const auto bytes = encode_process_info(pid, record);
        if (!memory_.copy_in(destination, bytes)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        destination += record_size;
      }
      if (!memory_.write32(registers[3], required)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      bsd_success(cpu, 0);
      return;
    }
    if (process_selector &&
        *process_selector == darwin::sysctl::kernel_process_id) {
      // CTL_KERN/KERN_PROC/KERN_PROC_PID.
      const auto kinfo_proc_size =
          darwin::sysctl::arm32_kernel_process_info_size;
      const auto requested_pid = memory_.read32(registers[0] + 12);
      if (!requested_pid || registers[3] == 0) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      const auto process = shared_state_->processes.find(*requested_pid);
      if (registers[2] == 0) {
        const auto required = process == shared_state_->processes.end()
                                  ? 5U * kinfo_proc_size
                                  : 6U * kinfo_proc_size;
        if (!memory_.write32(registers[3], required)) {
          bsd_error(cpu, bsd_support::bad_address);
        } else {
          bsd_success(cpu, 0);
        }
        return;
      }
      if (process == shared_state_->processes.end()) {
        if (!memory_.write32(registers[3], 0))
          bsd_error(cpu, bsd_support::bad_address);
        else
          bsd_success(cpu, 0);
        return;
      }
      if (*old_size < kinfo_proc_size) {
        if (!memory_.write32(registers[3], 0))
          bsd_error(cpu, bsd_support::bad_address);
        else
          bsd_error(cpu, 12); // ENOMEM
        return;
      }
      const auto &record = process->second;
      const auto bytes = encode_process_info(*requested_pid, record);
      if (!memory_.copy_in(registers[2], bytes) ||
          !memory_.write32(registers[3], kinfo_proc_size)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == 1 && *mib1 == 65) { // KERN_OSVERSION
      constexpr std::string_view build_version{"1A543a"};
      const auto required =
          static_cast<std::uint32_t>(build_version.size() + 1);
      if (!memory_.write32(registers[3], required)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[2] != 0) {
        if (*old_size < required) {
          bsd_error(cpu, 12); // ENOMEM
          return;
        }
        std::array<std::byte, 7> bytes{};
        for (std::size_t index = 0; index < build_version.size(); ++index) {
          bytes[index] = static_cast<std::byte>(build_version[index]);
        }
        if (!memory_.copy_in(registers[2], bytes)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == 1 && *mib1 == 61 && registers[1] == 3) { // KERN_TFP
      const auto selector = memory_.read32(registers[0] + 8);
      if (!selector || (*selector != 2 && *selector != 3) ||
          registers[4] == 0 || registers[5] != 4) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto group = memory_.read32(registers[4]);
      if (!group) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      shared_state_->task_for_pid_groups[*selector - 2] = *group;
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == darwin::sysctl::control_kernel &&
        *mib1 == darwin::sysctl::kernel_security_level &&
        registers[1] == 2) { // KERN_SECURELVL
      const auto previous =
          shared_state_->security_level.load(std::memory_order_relaxed);
      std::optional<std::int32_t> requested_level;
      if (registers[4] != 0) {
        if (registers[5] != sizeof(std::int32_t)) {
          bsd_error(cpu, bsd_support::invalid_argument);
          return;
        }
        const auto requested = memory_.read32(registers[4]);
        if (!requested) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        const auto level = static_cast<std::int32_t>(*requested);
        if (level < previous && process_.pid != 1) {
          bsd_error(cpu, darwin::error::operation_not_permitted);
          return;
        }
        requested_level = level;
      }
      if (registers[3] != 0) {
        constexpr std::uint32_t value_size = sizeof(std::int32_t);
        if (!memory_.write32(registers[3], value_size)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        if (registers[2] != 0) {
          if (*old_size < value_size) {
            bsd_error(cpu, darwin::error::no_memory);
            return;
          }
          if (!memory_.write32(registers[2],
                               static_cast<std::uint32_t>(previous))) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
          }
        }
      }
      if (requested_level) {
        shared_state_->security_level.store(*requested_level,
                                            std::memory_order_relaxed);
      }
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == 1 && *mib1 == 5) { // KERN_MAXVNODES (read/write)
      const auto previous = shared_state_->desired_vnodes;
      if (registers[4] != 0) {
        if (registers[5] != 4) {
          bsd_error(cpu, bsd_support::invalid_argument);
          return;
        }
        const auto requested = memory_.read32(registers[4]);
        if (!requested) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        shared_state_->desired_vnodes = *requested;
      }
      if (registers[3] != 0) {
        if (!memory_.write32(registers[3], 4) ||
            (registers[2] != 0 &&
             (*old_size < 4 || !memory_.write32(registers[2], previous)))) {
          bsd_error(cpu, *old_size < 4 ? 12 : bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == 1 && *mib1 == 10) { // KERN_HOSTNAME (read/write)
      const auto previous = shared_state_->hostname;
      if (registers[4] != 0) {
        if (registers[5] == 0 || registers[5] > 256) {
          bsd_error(cpu, bsd_support::invalid_argument);
          return;
        }
        const auto bytes = memory_.read_bytes(registers[4], registers[5]);
        if (!bytes) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        shared_state_->hostname.assign(
            reinterpret_cast<const char *>(bytes->data()), bytes->size());
        if (!shared_state_->hostname.empty() &&
            shared_state_->hostname.back() == '\0') {
          shared_state_->hostname.pop_back();
        }
      }
      if (registers[3] != 0) {
        const auto required = static_cast<std::uint32_t>(previous.size() + 1);
        if (!memory_.write32(registers[3], required)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        if (registers[2] != 0) {
          if (*old_size < required) {
            bsd_error(cpu, 12);
            return;
          }
          std::vector<std::byte> bytes(required);
          std::transform(
              previous.begin(), previous.end(), bytes.begin(),
              [](char value) { return static_cast<std::byte>(value); });
          if (!memory_.copy_in(registers[2], bytes)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
          }
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if (*mib0 == 6 && *mib1 == 24) { // HW_MEMSIZE
      if (registers[3] == 0 || !memory_.write32(registers[3], 8)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[2] != 0) {
        if (*old_size < 8) {
          bsd_error(cpu, 12);
          return;
        }
        if (!memory_.write64(registers[2], shared_state_->device_ram_bytes)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if (const auto hardware_string =
            *mib0 == darwin::sysctl::control_hardware
                ? darwin::sysctl::hardware_string(
                      *mib1, shared_state_->device_product_type,
                      shared_state_->device_board_config)
                : std::nullopt) {
      const auto required =
          static_cast<std::uint32_t>(hardware_string->size() + 1);
      if (registers[4] != 0) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
      }
      if (registers[3] == 0 || !memory_.write32(registers[3], required)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[2] != 0) {
        if (*old_size < required) {
          bsd_error(cpu, darwin::error::no_memory);
          return;
        }
        std::vector<std::byte> bytes(required);
        std::transform(
            hardware_string->begin(), hardware_string->end(), bytes.begin(),
            [](char value) { return static_cast<std::byte>(value); });
        if (!memory_.copy_in(registers[2], bytes)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    if ((*mib0 == 1 && (*mib1 == 6 || *mib1 == 7 || *mib1 == 8 || *mib1 == 35 ||
                        *mib1 == 40 || *mib1 == 54 || *mib1 == 66)) ||
        (*mib0 == 6 &&
         (*mib1 == 3 || *mib1 == 4 || *mib1 == 5 || *mib1 == 6 || *mib1 == 7 ||
          *mib1 == 11 || *mib1 == 13 || *mib1 == 25))) {
      // Common read-only Darwin 8 capacity/boot values plus HW_NCPU.
      if (registers[4] != 0) {
        bsd_error(cpu, 1); // EPERM: read-only MIB
        return;
      }
      if (!memory_.write32(registers[3], 4)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (registers[2] != 0) {
        if (*old_size < 4) {
          bsd_error(cpu, 12); // ENOMEM
          return;
        }
        std::uint32_t value = 1;
        if (*mib0 == 1) {
          switch (*mib1) {
          case 5:
            value = 65'536;
            break; // KERN_MAXVNODES
          case 6:
            value = 2'048;
            break; // KERN_MAXPROC
          case 7:
            value = 12'288;
            break; // KERN_MAXFILES
          case 8:
            value = 262'144;
            break; // KERN_ARGMAX
          case 35:
            value = 0x30000000U;
            break; // KERN_USRSTACK32
          case 40: // KERN_NETBOOT
          case 66:
            value = 0;
            break; // KERN_SAFEBOOT
          default:
            value = 1;
            break;
          }
        } else {
          switch (*mib1) {
          case 4:
            value = 1234;
            break; // HW_BYTEORDER
          case 5:
            value = 0x08000000U;
            break; // HW_PHYSMEM
          case 6:
            value = 0x07000000U;
            break; // HW_USERMEM
          case 7:
            value = 4096;
            break; // HW_PAGESIZE
          case 11:
            value = 1;
            break; // HW_FLOATINGPT
          case 13:
            value = 0;
            break; // HW_VECTORUNIT
          default:
            value = 1;
            break; // CPU counts
          }
        }
        if (!memory_.write32(registers[2], value)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
      }
      bsd_success(cpu, 0);
      return;
    }
    std::ostringstream message;
    message << "[sysctl] unsupported mib";
    for (std::uint32_t index = 0; index < registers[1] && index < 8; ++index) {
      message
          << ' '
          << memory_.read32(registers[0] + index * 4U).value_or(0xffffffffU);
    }
    message << '\n';
    output_.write(message.str());
    trace_unknown(cpu, "BSD syscall", number);
    bsd_error(cpu, bsd_support::not_implemented);
    return;
  }
  case 362: { // kqueue
    const auto fd = allocate_file_descriptor();
    if (!fd) {
      bsd_error(cpu, 24); // EMFILE
      return;
    }
    virtual_descriptors_.emplace(*fd, "kqueue");
    kqueues_.emplace(*fd, std::vector<KeventRegistration>{});
    bsd_success(cpu, *fd);
    return;
  }
  case 363: { // kevent
    const auto fd = registers[0];
    const auto queue = kqueues_.find(fd);
    if (queue == kqueues_.end() || registers[2] > 4096 || registers[4] > 4096) {
      bsd_error(cpu, queue == kqueues_.end() ? bsd_support::bad_file_descriptor
                                             : bsd_support::invalid_argument);
      return;
    }
    std::uint32_t receipts_written = 0;
    const auto write_receipt = [&](std::uint32_t ident,
                                   std::int16_t filter,
                                   std::uint16_t flags,
                                   std::uint32_t filter_flags,
                                   std::uint32_t data,
                                   std::uint32_t user_data) {
      if (receipts_written >= registers[4] || registers[3] == 0) {
        return false;
      }
      const auto event = registers[3] +
                         receipts_written *
                             darwin::kqueue::arm32_event::size;
      if (!memory_.write32(
              event + darwin::kqueue::arm32_event::identifier_offset, ident) ||
          !memory_.write16(
              event + darwin::kqueue::arm32_event::filter_offset,
              static_cast<std::uint16_t>(filter)) ||
          !memory_.write16(
              event + darwin::kqueue::arm32_event::flags_offset,
              flags | darwin::kqueue::event_error) ||
          !memory_.write32(
              event + darwin::kqueue::arm32_event::filter_flags_offset,
              filter_flags) ||
          !memory_.write32(event + darwin::kqueue::arm32_event::data_offset,
                           data) ||
          !memory_.write32(
              event + darwin::kqueue::arm32_event::user_data_offset,
              user_data)) {
        return false;
      }
      ++receipts_written;
      return true;
    };
    for (std::uint32_t index = 0; index < registers[2]; ++index) {
      const auto address =
          registers[1] + index * darwin::kqueue::arm32_event::size;
      const auto ident = memory_.read32(
          address + darwin::kqueue::arm32_event::identifier_offset);
      const auto filter =
          memory_.read16(address + darwin::kqueue::arm32_event::filter_offset);
      const auto flags =
          memory_.read16(address + darwin::kqueue::arm32_event::flags_offset);
      const auto filter_flags = memory_.read32(
          address + darwin::kqueue::arm32_event::filter_flags_offset);
      const auto data =
          memory_.read32(address + darwin::kqueue::arm32_event::data_offset);
      const auto user_data = memory_.read32(
          address + darwin::kqueue::arm32_event::user_data_offset);
      if (!ident || !filter || !flags || !filter_flags || !data || !user_data) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      const auto signed_filter = static_cast<std::int16_t>(*filter);
      auto found = std::find_if(queue->second.begin(), queue->second.end(),
                                [&](const auto &registration) {
                                  return registration.ident == *ident &&
                                         registration.filter == signed_filter;
                                });
      if ((*flags & darwin::kqueue::event_delete) != 0) {
        if (found != queue->second.end())
          queue->second.erase(found);
        if ((*flags & darwin::kqueue::event_receipt) != 0 &&
            !write_receipt(*ident, signed_filter, *flags, *filter_flags, 0,
                           *user_data)) {
          bsd_error(cpu, bsd_support::bad_address);
          return;
        }
        continue;
      }
      if ((*flags & darwin::kqueue::event_add) != 0) {
        KeventRegistration registration{
            *ident,
            signed_filter,
            *flags,
            *filter_flags,
            static_cast<std::int32_t>(*data),
            *user_data,
        };
        if (signed_filter == darwin::kqueue::filter_process) {
          std::lock_guard lock{shared_state_->mach_mutex};
          const auto process =
              shared_state_->process_kevent_states.find(*ident);
          if (process != shared_state_->process_kevent_states.end()) {
            registration.process_exec_generation =
                process->second.exec_generation;
            registration.process_exit_generation =
                process->second.exit_generation;
          }
        }
        if (found == queue->second.end()) {
          queue->second.push_back(registration);
        } else {
          *found = registration;
        }
      }
      if ((*flags & darwin::kqueue::event_receipt) != 0 &&
          !write_receipt(*ident, signed_filter, *flags, *filter_flags, 0,
                         *user_data)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
    }
    if (receipts_written != 0) {
      bsd_success(cpu, receipts_written);
      return;
    }
    std::optional<std::uint64_t> timeout_deadline;
    bool poll_only = false;
    if (registers[5] != 0) {
      const auto seconds_word = memory_.read32(
          registers[5] + darwin::kqueue::arm32_timespec::seconds_offset);
      const auto nanoseconds_word = memory_.read32(
          registers[5] + darwin::kqueue::arm32_timespec::nanoseconds_offset);
      if (!seconds_word || !nanoseconds_word) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      const auto seconds = static_cast<std::int32_t>(*seconds_word);
      const auto nanoseconds = static_cast<std::int32_t>(*nanoseconds_word);
      if (seconds < 0 || nanoseconds < 0 ||
          static_cast<std::uint64_t>(nanoseconds) >=
              darwin::kqueue::nanoseconds_per_second) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto duration = static_cast<std::uint64_t>(seconds) *
                                darwin::kqueue::nanoseconds_per_second +
                            static_cast<std::uint64_t>(nanoseconds);
      poll_only = duration == 0;
      const auto now = shared_state_->clock.now();
      timeout_deadline =
          duration > std::numeric_limits<std::uint64_t>::max() - now
              ? std::numeric_limits<std::uint64_t>::max()
              : now + duration;
    }
    if (registers[4] != 0) {
      const auto ready = collect_ready_kevents(fd, registers[3], registers[4]);
      if (!ready) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (*ready != 0) {
        bsd_success(cpu, *ready);
        return;
      }
    }
    if (registers[4] == 0) {
      bsd_success(cpu, 0);
      return;
    }
    if (poll_only) {
      bsd_success(cpu, 0);
      return;
    }
    process_.waiting_for_events = true;
    pending_kevents_[cpu.processor_id()] = PendingKevent{
        fd, registers[3], registers[4], cpu.processor_id(), timeout_deadline};
    output_.write("[network] kevent wait pid=" + std::to_string(process_.pid) +
                  " fd=" + std::to_string(fd) + " registrations=" +
                  std::to_string(queue->second.size()) + "\n");
    bsd_success(cpu, 0);
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
    return;
  }
  default:
    trace_unknown(cpu, "BSD syscall", number);
    return;
  }
}

} // namespace ilemu
