#include "ilemu/kernel.hpp"

#include "ilemu/baseband_device.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/kernel_network.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
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

#include <sys/stat.h>
#include <unistd.h>

#include "support.hpp"

namespace ilemu {
namespace {

constexpr std::uint32_t maximum_baseband_io_traces = 64;

} // namespace

void CompatibilityKernel::dispatch_bsd_descriptor_memory(Cpu &cpu,
                                                         std::uint32_t number) {
  auto &registers = cpu.registers();
  switch (number) {
  case darwin::syscall::read: {
    auto fd = registers[0];
    if (const auto duplicate = duplicated_descriptors_.find(fd);
        duplicate != duplicated_descriptors_.end()) {
      fd = duplicate->second;
    }
    const auto size = static_cast<std::size_t>(registers[2]);
    if (size > bsd_support::maximum_io) {
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
    const auto virtual_descriptor = virtual_descriptors_.find(fd);
    const auto baseband_descriptor =
        virtual_descriptor != virtual_descriptors_.end() &&
        virtual_descriptor->second == bsd::baseband_device::descriptor_kind;
    const auto readable_socket =
        host_sockets_.contains(fd) || virtual_udp_sockets_.contains(fd) ||
        bpf_descriptors_.contains(fd) ||
        kernel_control_endpoints_.contains(fd) ||
        socket_pair_endpoints_.contains(fd) || baseband_descriptor ||
        (virtual_descriptor != virtual_descriptors_.end() &&
         (virtual_descriptor->second == "system-event-socket" ||
          virtual_descriptor->second == "route-socket" ||
          virtual_descriptor->second ==
              darwin::network::apple80211_driver::event_descriptor_kind));
    if (readable_socket) {
      if (receive_socket_bytes(cpu, fd, registers[1],
                               static_cast<std::uint32_t>(size))) {
        return;
      }
      if ((file_status_flags_[fd] & darwin::open_flag::non_block) != 0) {
        bsd_error(cpu, bsd_support::would_block);
        return;
      }
      pending_socket_reads_[cpu.processor_id()] = PendingSocketRead{
          fd, registers[1],      static_cast<std::uint32_t>(size), 0,
          0,  cpu.processor_id()};
      process_.waiting_for_events = true;
      output_.write(
          std::string{baseband_descriptor ? "[baseband]" : "[network]"} +
          " read wait pid=" + std::to_string(process_.pid) + " fd=" +
          std::to_string(fd) + " bytes=" + std::to_string(size) + "\n");
      cpu.halt(Dynarmic::HaltReason::UserDefined5);
      return;
    }
    std::vector<std::byte> bytes(size);
    if (const auto device = virtual_descriptors_.find(fd);
        device != virtual_descriptors_.end() && device->second == "random") {
      for (auto &byte : bytes) {
        random_state_ ^= random_state_ << 13U;
        random_state_ ^= random_state_ >> 7U;
        random_state_ ^= random_state_ << 17U;
        byte = static_cast<std::byte>(random_state_ & 0xffU);
      }
    } else if (const auto resolver = virtual_descriptors_.find(fd);
               resolver != virtual_descriptors_.end() &&
               resolver->second == "resolver-config") {
      constexpr std::string_view configuration{"nameserver 10.0.2.3\n"};
      const auto offset = std::min<std::size_t>(
          file_offsets_[fd], configuration.size());
      const auto count =
          std::min<std::size_t>(size, configuration.size() - offset);
      bytes.resize(count);
      const auto begin = configuration.begin() +
                         static_cast<std::ptrdiff_t>(offset);
      std::transform(begin,
                     begin + static_cast<std::ptrdiff_t>(count),
                     bytes.begin(), [](char value) {
                       return static_cast<std::byte>(value);
                     });
      file_offsets_[fd] = offset + count;
    } else if (const auto console_device = virtual_descriptors_.find(fd);
               console_device != virtual_descriptors_.end() &&
               console_device->second == "console") {
      bytes.clear();
    } else if (const auto file = file_descriptors_.find(fd);
               file != file_descriptors_.end()) {
      const auto description = ensure_regular_file_open_description(fd);
      if (!description) {
        bsd_error(cpu, darwin::error::bad_file_descriptor);
        return;
      }
      const auto result = ::pread(
          description->host_descriptor(), bytes.data(), bytes.size(),
          static_cast<off_t>(file_offsets_[fd]));
      if (result < 0) {
        bsd_error(cpu, bsd_support::darwin_filesystem_error(
                           std::error_code{errno, std::generic_category()}));
        return;
      }
      bytes.resize(static_cast<std::size_t>(result));
      file_offsets_[fd] += bytes.size();
    } else if (fd == 0) {
      bytes.clear(); // terminal input currently presents non-blocking EOF
    } else {
      bsd_error(cpu, bsd_support::bad_file_descriptor);
      return;
    }
    if (!memory_.copy_in(registers[1], bytes)) {
      bsd_error(cpu, bsd_support::bad_address);
      return;
    }
    bsd_success(cpu, static_cast<std::uint32_t>(bytes.size()));
    return;
  }
  case darwin::syscall::write: {
    auto fd = registers[0];
    if (const auto duplicate = duplicated_descriptors_.find(fd);
        duplicate != duplicated_descriptors_.end()) {
      fd = duplicate->second;
    }
    const auto address = registers[1];
    const auto size = static_cast<std::size_t>(registers[2]);
    if (bpf_descriptors_.contains(fd)) {
      static_cast<void>(write_bpf_bytes(
          cpu, fd, address, static_cast<std::uint32_t>(size)));
      return;
    }
    if (kernel_control_endpoints_.contains(fd)) {
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      static_cast<void>(write_kernel_control_socket(cpu, fd, *bytes));
      return;
    }
    if (const auto descriptor = virtual_descriptors_.find(fd);
        descriptor != virtual_descriptors_.end() &&
        descriptor->second == "route-socket") {
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
      }
      const auto parsed = darwin::route::parse_message(*bytes);
      if (!parsed.message) {
        const auto parse_error =
            parsed.error == darwin::route::ParseError::UnsupportedVersion
                ? darwin::error::protocol_not_supported
                : (parsed.error == darwin::route::ParseError::UnsupportedType
                       ? darwin::error::operation_not_supported
                       : darwin::error::invalid_argument);
        bsd_error(cpu, parse_error);
        return;
      }
      auto entry = darwin::route::make_entry(*parsed.message);
      if (!entry) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
      }

      const auto query =
          parsed.message->type == darwin::route::message_get ||
          parsed.message->type == darwin::route::message_get_silent;
      std::optional<darwin::route::Entry> queried_entry;
      std::uint32_t route_error = !query && process_.effective_uid != 0
                                      ? darwin::error::operation_not_permitted
                                      : 0U;
      if (query) {
        queried_entry = shared_state_->route_table.lookup(*entry);
        if (!queried_entry) {
          route_error = darwin::error::no_such_process;
        }
      } else if (route_error == 0) {
        const auto add_or_change =
            parsed.message->type == darwin::route::message_add ||
            parsed.message->type == darwin::route::message_change;
        if (add_or_change && entry->interface_name.empty() &&
            entry->interface_index == 0 && entry->gateway.size() >= 8U &&
            std::to_integer<std::uint8_t>(entry->gateway[1]) ==
                darwin::network::address_family_link) {
          const auto link_index = static_cast<std::uint16_t>(
              std::to_integer<std::uint8_t>(entry->gateway[2]) |
              (static_cast<std::uint16_t>(
                   std::to_integer<std::uint8_t>(entry->gateway[3]))
               << 8U));
          const auto name_length = static_cast<std::size_t>(
              std::to_integer<std::uint8_t>(entry->gateway[5]));
          if (name_length <= entry->gateway.size() - 8U) {
            entry->interface_index = link_index;
            entry->interface_name.assign(
                reinterpret_cast<const char*>(entry->gateway.data() + 8U),
                name_length);
          }
        }

        if (!entry->interface_name.empty() ||
            entry->interface_index != 0) {
          std::lock_guard network_lock{shared_state_->network_mutex};
          if (!entry->interface_name.empty()) {
            const auto interface =
                shared_state_->network_interfaces.find(
                    entry->interface_name);
            if (interface == shared_state_->network_interfaces.end()) {
              route_error = darwin::error::no_such_device_or_address;
            } else if (entry->interface_index != 0 &&
                       entry->interface_index !=
                           interface->second.index) {
              route_error = darwin::error::invalid_argument;
            } else {
              entry->interface_index = interface->second.index;
            }
          } else {
            const auto interface = std::find_if(
                shared_state_->network_interfaces.begin(),
                shared_state_->network_interfaces.end(),
                [&](const auto& candidate) {
                  return candidate.second.index ==
                         entry->interface_index;
                });
            if (interface == shared_state_->network_interfaces.end()) {
              route_error = darwin::error::no_such_device_or_address;
            } else {
              entry->interface_name = interface->first;
            }
          }
        }

        const auto gateway_family =
            entry->gateway.size() >= 2U
                ? std::to_integer<std::uint8_t>(entry->gateway[1])
                : darwin::network::address_family_unspecified;
        if (route_error == 0 && add_or_change &&
            entry->interface_name.empty() &&
            entry->interface_index == 0 &&
            (gateway_family ==
                 darwin::network::address_family_inet ||
             gateway_family ==
                 darwin::network::address_family_inet6)) {
          darwin::route::Entry gateway_query;
          gateway_query.family =
              static_cast<std::uint8_t>(gateway_family);
          gateway_query.destination = entry->gateway;
          if (const auto route =
                  shared_state_->route_table.lookup_bound_interface(
                      gateway_query)) {
            entry->interface_name = route->interface_name;
            entry->interface_index = route->interface_index;
          }
        }
      }
      if (!query && route_error == 0) {
        switch (
            shared_state_->route_table.apply(parsed.message->type, *entry)) {
        case darwin::route::ApplyResult::Applied:
          break;
        case darwin::route::ApplyResult::AlreadyExists:
          route_error = darwin::error::file_exists;
          break;
        case darwin::route::ApplyResult::NotFound:
          route_error = darwin::error::no_such_process;
          break;
        }
      }

      const auto include_interface =
          (parsed.message->addresses & darwin::route::address_interface) != 0;
      auto response =
          queried_entry
              ? darwin::route::make_entry_message(*queried_entry, process_.pid,
                                                  parsed.message->sequence,
                                                  true, include_interface)
              : darwin::route::make_response(*bytes, process_.pid, route_error,
                                             entry->interface_index);
      if (parsed.message->type == darwin::route::message_get_silent &&
          response.size() >= darwin::route::message_header_size) {
        response[3] = static_cast<std::byte>(darwin::route::message_get);
      }
      const auto silent =
          parsed.message->type == darwin::route::message_get_silent;
      const auto receiver_socket =
          silent && route_socket_states_.contains(fd) &&
                  route_socket_states_.at(fd)
              ? std::optional<std::uint64_t>{route_socket_states_.at(fd)
                                                 ->identifier}
              : std::nullopt;
      post_route_message(std::move(response), entry->family, receiver_socket);
      output_.write("[network] route pid=" + std::to_string(process_.pid) +
                    " type=" + std::to_string(parsed.message->type) +
                    " family=" + std::to_string(entry->family) +
                    (entry->interface_name.empty()
                         ? std::string{}
                         : " if=" + entry->interface_name) +
                    " error=" + std::to_string(route_error) + "\n");
      if (route_error != 0) {
        bsd_error(cpu, route_error);
      } else {
        bsd_success(cpu, static_cast<std::uint32_t>(bytes->size()));
      }
      return;
    }
    if (const auto host = host_sockets_.find(fd); host != host_sockets_.end()) {
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      const auto sent = host->second->send(*bytes);
      if (sent.status == HostSocketStatus::WouldBlock) {
        bsd_error(cpu, bsd_support::would_block);
      } else if (sent.status == HostSocketStatus::Error) {
        bsd_error(cpu, sent.darwin_error);
      } else {
        bsd_success(cpu, static_cast<std::uint32_t>(sent.transferred));
      }
      return;
    }
    if (const auto udp = virtual_udp_sockets_.find(fd);
        udp != virtual_udp_sockets_.end()) {
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      const auto sent = udp->second->send(*bytes);
      if (sent != bsd::VirtualUdpStatus::Success) {
        bsd_error(cpu, sent == bsd::VirtualUdpStatus::NotConnected
                           ? bsd_support::not_connected
                           : bsd_support::invalid_argument);
      } else {
        bsd_success(cpu, static_cast<std::uint32_t>(bytes->size()));
      }
      return;
    }
    if (const auto endpoint = socket_pair_endpoints_.find(fd);
        endpoint != socket_pair_endpoints_.end()) {
      if (!endpoint->second.local_write_open() ||
          !endpoint->second.peer_read_open()) {
        bsd_error(cpu, darwin::error::broken_pipe);
        return;
      }
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (socket_payload_trace_count_ < 32U) {
        output_.write("[network] write pid=" + std::to_string(process_.pid) +
                      " fd=" + std::to_string(fd) +
                      " pair=" + std::to_string(endpoint->second.pair) +
                      " bytes=" + std::to_string(bytes->size()) + " hex=" +
                      bsd_support::format_payload_prefix(*bytes) + "\n");
        ++socket_payload_trace_count_;
      }
      std::lock_guard socket_lock{shared_state_->socket_mutex};
      auto &destination =
          shared_state_->socket_pair_buffers[endpoint->second.pair]
                                            [1U - endpoint->second.side];
      destination.insert(destination.end(), bytes->begin(), bytes->end());
      bsd_success(cpu, static_cast<std::uint32_t>(bytes->size()));
      return;
    }
    if (const auto device = virtual_descriptors_.find(fd);
        device != virtual_descriptors_.end() && device->second == "console") {
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      output_.write(std::string_view{
          reinterpret_cast<const char *>(bytes->data()), bytes->size()});
      bsd_success(cpu, static_cast<std::uint32_t>(bytes->size()));
      return;
    }
    if (const auto device = virtual_descriptors_.find(fd);
        device != virtual_descriptors_.end() &&
        device->second == bsd::baseband_device::descriptor_kind) {
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      if (baseband_io_trace_count_ < maximum_baseband_io_traces) {
        output_.write("[baseband] write pid=" + std::to_string(process_.pid) +
                      " fd=" + std::to_string(fd) +
                      " bytes=" + std::to_string(bytes->size()) + " hex=" +
                      bsd_support::format_payload_prefix(*bytes) + "\n");
        ++baseband_io_trace_count_;
      }
      bsd_success(cpu, static_cast<std::uint32_t>(
                           shared_state_->baseband_device_state.write(*bytes)));
      return;
    }
    if (const auto file = file_descriptors_.find(fd);
        file != file_descriptors_.end()) {
      const auto flags = file_status_flags_.contains(fd)
                             ? file_status_flags_.at(fd)
                             : darwin::open_flag::read_only;
      if ((flags & darwin::open_flag::access_mode) ==
          darwin::open_flag::read_only) {
        bsd_error(cpu, darwin::error::bad_file_descriptor);
        return;
      }
      if (size > bsd_support::maximum_io) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
      }
      const auto bytes = memory_.read_bytes(address, size);
      if (!bytes) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
      }
      const auto description = ensure_regular_file_open_description(fd);
      if (!description) {
        bsd_error(cpu, darwin::error::bad_file_descriptor);
        return;
      }
      std::lock_guard filesystem_lock{shared_state_->filesystem_mutex};
      std::uint64_t position = file_offsets_[fd];
      if ((flags & darwin::open_flag::append) != 0) {
        struct stat status {};
        if (::fstat(description->host_descriptor(), &status) != 0) {
          bsd_error(cpu, bsd_support::darwin_filesystem_error(
                             std::error_code{errno,
                                             std::generic_category()}));
          return;
        }
        position = static_cast<std::uint64_t>(status.st_size);
      }
      const auto result = ::pwrite(description->host_descriptor(),
                                   bytes->data(), bytes->size(),
                                   static_cast<off_t>(position));
      if (result < 0) {
        bsd_error(cpu, bsd_support::darwin_filesystem_error(
                           std::error_code{errno, std::generic_category()}));
        return;
      }
      file_offsets_[fd] = position + static_cast<std::size_t>(result);
      bsd_success(cpu, static_cast<std::uint32_t>(result));
      return;
    }
    if (fd != 1 && fd != 2) {
      bsd_error(cpu, bsd_support::bad_file_descriptor);
      return;
    }
    if (size > bsd_support::maximum_io) {
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
    const auto bytes = memory_.read_bytes(address, size);
    if (!bytes) {
      bsd_error(cpu, bsd_support::bad_address);
      return;
    }
    output_.write(std::string_view{
        reinterpret_cast<const char *>(bytes->data()), bytes->size()});
    bsd_success(cpu, static_cast<std::uint32_t>(bytes->size()));
    return;
  }
  case 41: { // dup
    const auto source = registers[0];
    const bool valid = source <= 2 || file_descriptors_.contains(source) ||
                       virtual_descriptors_.contains(source) ||
                       duplicated_descriptors_.contains(source);
    if (!valid) {
      bsd_error(cpu, bsd_support::bad_file_descriptor);
      return;
    }
    const auto destination = allocate_file_descriptor();
    if (!destination) {
      bsd_error(cpu, 24); // EMFILE
      return;
    }
    const auto allocated = *destination;
    if (const auto file = file_descriptors_.find(source);
        file != file_descriptors_.end()) {
      file_descriptors_.emplace(allocated, file->second);
      file_offsets_[allocated] = file_offsets_[source];
      file_status_flags_[allocated] = file_status_flags_[source];
      if (const auto description = regular_file_open_descriptions_.find(source);
          description != regular_file_open_descriptions_.end()) {
        regular_file_open_descriptions_[allocated] = description->second;
      }
      if (const auto block = virtual_block_descriptors_.find(source);
          block != virtual_block_descriptors_.end()) {
        virtual_block_descriptors_[allocated] = block->second;
      }
    } else if (const auto device = virtual_descriptors_.find(source);
               device != virtual_descriptors_.end()) {
      virtual_descriptors_.emplace(allocated, device->second);
      if (const auto offset = file_offsets_.find(source);
          offset != file_offsets_.end()) {
        file_offsets_[allocated] = offset->second;
      }
      if (const auto host = host_sockets_.find(source);
          host != host_sockets_.end()) {
        host_sockets_[allocated] = host->second;
      }
      if (const auto socket = virtual_udp_sockets_.find(source);
          socket != virtual_udp_sockets_.end()) {
        virtual_udp_sockets_[allocated] = socket->second;
      }
      if (const auto control = kernel_control_endpoints_.find(source);
          control != kernel_control_endpoints_.end()) {
        kernel_control_endpoints_[allocated] = control->second;
      }
      if (const auto flags = file_status_flags_.find(source);
          flags != file_status_flags_.end()) {
        file_status_flags_[allocated] = flags->second;
      }
      if (const auto options = socket_options_.find(source);
          options != socket_options_.end()) {
        socket_options_[allocated] = options->second;
      }
      if (const auto bound = bound_socket_names_.find(source);
          bound != bound_socket_names_.end()) {
        bound_socket_names_[allocated] = bound->second;
      }
      if (listening_sockets_.contains(source)) {
        listening_sockets_.insert(allocated);
      }
      if (const auto endpoint = socket_pair_endpoints_.find(source);
          endpoint != socket_pair_endpoints_.end()) {
        socket_pair_endpoints_[allocated] = endpoint->second;
      }
      if (const auto listener = unix_listener_states_.find(source);
          listener != unix_listener_states_.end()) {
        unix_listener_states_[allocated] = listener->second;
      }
      if (const auto filter = system_event_filters_.find(source);
          filter != system_event_filters_.end()) {
        system_event_filters_[allocated] = filter->second;
      }
      if (const auto cursor = system_event_next_identifiers_.find(source);
          cursor != system_event_next_identifiers_.end()) {
        system_event_next_identifiers_[allocated] = cursor->second;
      }
      if (const auto state = route_socket_states_.find(source);
          state != route_socket_states_.end()) {
        route_socket_states_[allocated] = state->second;
      }
      if (const auto bpf = bpf_descriptors_.find(source);
          bpf != bpf_descriptors_.end()) {
        bpf_descriptors_[allocated] = bpf->second;
      }
    } else {
      const auto original = duplicated_descriptors_.contains(source)
                                ? duplicated_descriptors_.at(source)
                                : source;
      duplicated_descriptors_.emplace(allocated, original);
    }
    bsd_success(cpu, allocated);
    return;
  }
  case 42: { // pipe: Darwin returns the two descriptors in retval[0:1]
    const auto read_fd = allocate_file_descriptor();
    if (!read_fd) {
      bsd_error(cpu, 24); // EMFILE
      return;
    }
    virtual_descriptors_.emplace(*read_fd, "pipe-read");
    file_status_flags_[*read_fd] = darwin::open_flag::read_only;
    descriptor_flags_[*read_fd] = 0;
    const auto write_fd = allocate_file_descriptor();
    if (!write_fd) {
      virtual_descriptors_.erase(*read_fd);
      file_status_flags_.erase(*read_fd);
      descriptor_flags_.erase(*read_fd);
      bsd_error(cpu, 24);
      return;
    }
    virtual_descriptors_.emplace(*write_fd, "pipe-write");
    file_status_flags_[*write_fd] = darwin::open_flag::write_only;
    descriptor_flags_[*write_fd] = 0;
    const auto pair = shared_state_->next_socket_pair++;
    auto endpoints = make_socket_pair_endpoints(pair);
    shared_state_->socket_pair_buffers.emplace(
        pair, std::array<std::deque<std::byte>, 2>{});
    socket_pair_endpoints_[*read_fd] = std::move(endpoints.first);
    socket_pair_endpoints_[*write_fd] = std::move(endpoints.second);
    output_.write("[network] pipe pid=" + std::to_string(process_.pid) +
                  " pair=" + std::to_string(pair) +
                  " fds=" + std::to_string(*read_fd) + "," +
                  std::to_string(*write_fd) + "\n");
    bsd_success(cpu, *read_fd, *write_fd);
    return;
  }
  case 73: // munmap
    if (registers[1] == 0 || !memory_.unmap(registers[0], registers[1])) {
      bsd_error(cpu, bsd_support::invalid_argument);
    } else {
      bsd_success(cpu, 0);
    }
    return;
  case darwin::syscall::get_descriptor_table_size:
    bsd_success(cpu, file_descriptor_limit());
    return;
  case darwin::syscall::duplicate_to: {
    const auto source = registers[0];
    const auto destination = registers[1];
    const bool valid = source <= 2 || file_descriptors_.contains(source) ||
                       virtual_descriptors_.contains(source) ||
                       duplicated_descriptors_.contains(source);
    if (!valid || destination >= file_descriptor_limit()) {
      bsd_error(cpu, bsd_support::bad_file_descriptor);
      return;
    }
    if (source == destination) {
      bsd_success(cpu, destination);
      return;
    }
    // XNU attaches descriptor filters to an fd-table entry. Replacing the
    // destination closes that entry and detaches every associated knote.
    detach_kevents_for_descriptor(destination);
    release_record_locks_for_descriptor(destination);
    file_descriptors_.erase(destination);
    file_offsets_.erase(destination);
    regular_file_open_descriptions_.erase(destination);
    file_status_flags_.erase(destination);
    virtual_block_descriptors_.erase(destination);
    bpf_descriptors_.erase(destination);
    virtual_descriptors_.erase(destination);
    host_sockets_.erase(destination);
    pending_wifi_driver_events_.erase(destination);
    virtual_udp_sockets_.erase(destination);
    kernel_control_endpoints_.erase(destination);
    duplicated_descriptors_.erase(destination);
    socket_pair_endpoints_.erase(destination);
    unix_listener_states_.erase(destination);
    bound_socket_names_.erase(destination);
    listening_sockets_.erase(destination);
    descriptor_flags_.erase(destination);
    socket_options_.erase(destination);
    system_event_filters_.erase(destination);
    apple80211_scan_delivered_.erase(destination);
    system_event_next_identifiers_.erase(destination);
    route_socket_states_.erase(destination);
    kqueues_.erase(destination);
    if (const auto file = file_descriptors_.find(source);
        file != file_descriptors_.end()) {
      file_descriptors_[destination] = file->second;
      file_offsets_[destination] = file_offsets_[source];
      file_status_flags_[destination] = file_status_flags_[source];
      if (const auto description = regular_file_open_descriptions_.find(source);
          description != regular_file_open_descriptions_.end()) {
        regular_file_open_descriptions_[destination] = description->second;
      }
      if (const auto block = virtual_block_descriptors_.find(source);
          block != virtual_block_descriptors_.end()) {
        virtual_block_descriptors_[destination] = block->second;
      }
    } else if (const auto device = virtual_descriptors_.find(source);
               device != virtual_descriptors_.end()) {
      virtual_descriptors_[destination] = device->second;
      if (const auto offset = file_offsets_.find(source);
          offset != file_offsets_.end()) {
        file_offsets_[destination] = offset->second;
      }
      if (const auto host = host_sockets_.find(source);
          host != host_sockets_.end()) {
        host_sockets_[destination] = host->second;
      }
      if (const auto socket = virtual_udp_sockets_.find(source);
          socket != virtual_udp_sockets_.end()) {
        virtual_udp_sockets_[destination] = socket->second;
      }
      if (const auto control = kernel_control_endpoints_.find(source);
          control != kernel_control_endpoints_.end()) {
        kernel_control_endpoints_[destination] = control->second;
      }
      if (const auto flags = file_status_flags_.find(source);
          flags != file_status_flags_.end()) {
        file_status_flags_[destination] = flags->second;
      }
      if (const auto options = socket_options_.find(source);
          options != socket_options_.end()) {
        socket_options_[destination] = options->second;
      }
      if (const auto bound = bound_socket_names_.find(source);
          bound != bound_socket_names_.end()) {
        bound_socket_names_[destination] = bound->second;
      }
      if (listening_sockets_.contains(source)) {
        listening_sockets_.insert(destination);
      }
      if (const auto endpoint = socket_pair_endpoints_.find(source);
          endpoint != socket_pair_endpoints_.end()) {
        socket_pair_endpoints_[destination] = endpoint->second;
      }
      if (const auto listener = unix_listener_states_.find(source);
          listener != unix_listener_states_.end()) {
        unix_listener_states_[destination] = listener->second;
      }
      if (const auto filter = system_event_filters_.find(source);
          filter != system_event_filters_.end()) {
        system_event_filters_[destination] = filter->second;
      }
      if (const auto cursor = system_event_next_identifiers_.find(source);
          cursor != system_event_next_identifiers_.end()) {
        system_event_next_identifiers_[destination] = cursor->second;
      }
      if (const auto state = route_socket_states_.find(source);
          state != route_socket_states_.end()) {
        route_socket_states_[destination] = state->second;
      }
      if (const auto bpf = bpf_descriptors_.find(source);
          bpf != bpf_descriptors_.end()) {
        bpf_descriptors_[destination] = bpf->second;
      }
    } else {
      duplicated_descriptors_[destination] =
          duplicated_descriptors_.contains(source)
              ? duplicated_descriptors_.at(source)
              : source;
    }
    bsd_success(cpu, destination);
    return;
  }
  case darwin::syscall::fcntl: {
    const auto fd = registers[0];
    const bool valid = fd <= 2 || file_descriptors_.contains(fd) ||
                       virtual_descriptors_.contains(fd) ||
                       duplicated_descriptors_.contains(fd);
    if (!valid) {
      bsd_error(cpu, bsd_support::bad_file_descriptor);
      return;
    }
    if (dispatch_bsd_record_locking(cpu, registers[1]))
      return;
    switch (registers[1]) {
    case darwin::fcntl_command::get_descriptor_flags:
      bsd_success(cpu, descriptor_flags_[fd]);
      return;
    case darwin::fcntl_command::set_descriptor_flags:
      descriptor_flags_[fd] = registers[2] & 1U; // FD_CLOEXEC
      bsd_success(cpu, 0);
      return;
    case darwin::fcntl_command::get_status_flags:
      bsd_success(cpu, file_status_flags_.contains(fd)
                           ? file_status_flags_.at(fd)
                           : (virtual_descriptors_.contains(fd) ? 2U : 0U));
      return;
    case darwin::fcntl_command::set_status_flags:
      if (file_status_flags_.contains(fd)) {
        constexpr std::uint32_t mutable_status_flags =
            darwin::open_flag::append | darwin::open_flag::non_block;
        file_status_flags_[fd] =
            (file_status_flags_[fd] & ~mutable_status_flags) |
            (registers[2] & mutable_status_flags);
      }
      bsd_success(cpu, 0);
      return;
    case darwin::fcntl_command::set_read_ahead:
    case darwin::fcntl_command::set_no_cache:
      // Host page-cache policy is deliberately not projected into guest
      // semantics. Both commands are advisory and have no visible file-data
      // effect, so accepting the requested Boolean policy is sufficient.
      bsd_success(cpu, 0);
      return;
    default:
      output_.write("[vfs] unsupported fcntl pid=" +
                    std::to_string(process_.pid) + " fd=" +
                    std::to_string(fd) + " command=" +
                    std::to_string(registers[1]) + "\n");
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
  }
  case darwin::syscall::memory_protect: { // mprotect
    const auto address = registers[0];
    const auto size = registers[1];
    const auto protection = registers[2];
    if ((address & (AddressSpace::page_size - 1U)) != 0 || size == 0 ||
        size - 1U > std::numeric_limits<std::uint32_t>::max() - address ||
        (protection & ~7U) != 0) {
      bsd_error(cpu, darwin::error::invalid_argument);
      return;
    }
    MemoryPermission permissions = MemoryPermission::None;
    if ((protection & 1U) != 0)
      permissions |= MemoryPermission::Read;
    if ((protection & 2U) != 0)
      permissions |= MemoryPermission::Write;
    if ((protection & 4U) != 0)
      permissions |= MemoryPermission::Execute;
    if (!memory_.protect(address, size, permissions)) {
      bsd_error(cpu, darwin::error::no_memory);
      return;
    }
    // Discard translations on the calling virtual CPU that may have been
    // compiled with the old execute permission.
    cpu.clear_cache();
    bsd_success(cpu, 0);
    return;
  }
  case 197: { // mmap
    auto address = registers[0];
    const auto size = registers[1];
    const auto protection = registers[2];
    const auto flags = registers[3];
    const auto fd = registers[4];
    const auto offset = static_cast<std::uint64_t>(registers[5]) |
                        (static_cast<std::uint64_t>(registers[6]) << 32U);
    if (size == 0 || size > bsd_support::maximum_io) {
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
    const auto overlaps = [&](std::uint32_t candidate) {
      for (std::uint64_t page = 0; page < size;
           page += AddressSpace::page_size) {
        if (memory_.mapped(candidate + static_cast<std::uint32_t>(page))) {
          return true;
        }
      }
      return false;
    };
    if ((flags & darwin::map_flag::fixed) == 0) {
      if (address == 0 || overlaps(address)) {
        address = 0x10000000U;
        while (overlaps(address))
          address += AddressSpace::page_size;
      }
    } else {
      memory_.unmap(address, size);
    }
    MemoryPermission permissions = MemoryPermission::None;
    if ((protection & 1U) != 0)
      permissions |= MemoryPermission::Read;
    if ((protection & 2U) != 0)
      permissions |= MemoryPermission::Write;
    if ((protection & 4U) != 0)
      permissions |= MemoryPermission::Execute;
    if (!memory_.map(address, size, permissions)) {
      bsd_error(cpu, 12); // ENOMEM
      return;
    }
    if ((flags & darwin::map_flag::anonymous) == 0) {
      const auto found = file_descriptors_.find(fd);
      if (found == file_descriptors_.end()) {
        bsd_error(cpu, bsd_support::bad_file_descriptor);
        return;
      }
      std::ifstream stream{found->second, std::ios::binary};
      stream.seekg(static_cast<std::streamoff>(offset));
      if (!stream) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      std::vector<std::byte> bytes(size);
      stream.read(reinterpret_cast<char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
      bytes.resize(static_cast<std::size_t>(stream.gcount()));
      if (!memory_.copy_in(address, bytes)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      static_cast<void>(userland_hle_.install_mapped_image(
          cpu, process_.pid, found->second, address, size, offset));
      if (mapping_trace_count_ < 64U) {
        output_.write("[mmap] pid=" + std::to_string(process_.pid) +
                      " address=" + std::to_string(address) +
                      " size=" + std::to_string(size) +
                      " offset=" + std::to_string(offset) +
                      " prot=" + std::to_string(protection) +
                      " flags=" + std::to_string(flags) +
                      " file=" + found->second.string() + "\n");
        ++mapping_trace_count_;
      }
    } else if (mapping_trace_count_ < 64U) {
      output_.write("[mmap] pid=" + std::to_string(process_.pid) + " address=" +
                    std::to_string(address) + " size=" + std::to_string(size) +
                    " offset=" + std::to_string(offset) +
                    " prot=" + std::to_string(protection) +
                    " flags=" + std::to_string(flags) + " anonymous\n");
      ++mapping_trace_count_;
    }
    bsd_success(cpu, address);
    return;
  }
  case 266: { // shm_open
    const auto name = memory_.read_c_string(registers[0], 256);
    if (!name) {
      bsd_error(cpu, bsd_support::bad_address);
      return;
    }
    auto object_name = *name;
    if (!object_name.empty() && object_name.front() == '/') {
      object_name.erase(object_name.begin());
    }
    // Darwin 8's own notifyd uses "apple.shm.notification_center"
    // without the POSIX-leading slash, so accept both spellings while
    // still rejecting path traversal and hierarchical names.
    if (object_name.empty() || object_name.find('/') != std::string::npos) {
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
    constexpr std::uint32_t o_creat = 0x0200U;
    constexpr std::uint32_t o_trunc = 0x0400U;
    constexpr std::uint32_t o_excl = 0x0800U;
    std::filesystem::path backing;
    bool created = false;
    {
      std::lock_guard filesystem_lock{shared_state_->filesystem_mutex};
      const auto existing =
          shared_state_->shared_memory_objects.find(object_name);
      if (existing != shared_state_->shared_memory_objects.end()) {
        if ((registers[1] & (o_creat | o_excl)) == (o_creat | o_excl)) {
          bsd_error(cpu, 17); // EEXIST
          return;
        }
        backing = existing->second;
      } else {
        if ((registers[1] & o_creat) == 0 || rootfs_.empty()) {
          bsd_error(cpu, 2); // ENOENT
          return;
        }
        const auto directory = rootfs_.parent_path() / "runtime" / "shm";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
          bsd_error(cpu, error == std::errc::permission_denied ? 13U : 5U);
          return;
        }
        backing = directory /
                  (std::to_string(shared_state_->next_shared_memory_object++) +
                   ".shm");
        std::ofstream create{backing, std::ios::binary | std::ios::trunc};
        if (!create) {
          bsd_error(cpu, 5); // EIO
          return;
        }
        shared_state_->shared_memory_objects.emplace(object_name, backing);
        created = true;
      }
    }
    if (!created && (registers[1] & o_trunc) != 0) {
      std::error_code error;
      std::filesystem::resize_file(backing, 0, error);
      if (error) {
        bsd_error(cpu, 5);
        return;
      }
    }
    const auto fd = allocate_file_descriptor();
    if (!fd) {
      bsd_error(cpu, 24); // EMFILE
      return;
    }
    file_descriptors_[*fd] = backing;
    file_offsets_[*fd] = 0;
    file_status_flags_[*fd] = registers[1];
    descriptor_flags_[*fd] = 0;
    static_cast<void>(ensure_regular_file_open_description(*fd));
    output_.write("[vfs] shm_open " + object_name +
                  " fd=" + std::to_string(*fd) + "\n");
    bsd_success(cpu, *fd);
    return;
  }
  case 267: { // shm_unlink
    const auto name = memory_.read_c_string(registers[0], 256);
    if (!name) {
      bsd_error(cpu, bsd_support::bad_address);
      return;
    }
    auto object_name = *name;
    if (!object_name.empty() && object_name.front() == '/') {
      object_name.erase(object_name.begin());
    }
    if (object_name.empty() || object_name.find('/') != std::string::npos) {
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
    bool erased = false;
    {
      std::lock_guard filesystem_lock{shared_state_->filesystem_mutex};
      erased = shared_state_->shared_memory_objects.erase(object_name) != 0;
    }
    if (!erased) {
      bsd_error(cpu, 2); // ENOENT
      return;
    }
    // Keep the backing inode until process teardown so descriptors and
    // mappings opened before unlink remain valid, as POSIX requires.
    output_.write("[vfs] shm_unlink " + object_name + "\n");
    bsd_success(cpu, 0);
    return;
  }
  default:
    trace_unknown(cpu, "BSD syscall", number);
    return;
  }
}

} // namespace ilemu
