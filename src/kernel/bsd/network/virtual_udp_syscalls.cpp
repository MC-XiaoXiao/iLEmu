#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"

#include "../support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ilemu {
namespace {

std::uint32_t socket_error(bsd::VirtualUdpStatus status) {
  switch (status) {
  case bsd::VirtualUdpStatus::NotConnected:
    return bsd_support::not_connected;
  case bsd::VirtualUdpStatus::AlreadyConnected:
    return bsd_support::already_connected;
  case bsd::VirtualUdpStatus::AddressFamilyUnsupported:
    return 47; // EAFNOSUPPORT
  case bsd::VirtualUdpStatus::InvalidArgument:
    return bsd_support::invalid_argument;
  case bsd::VirtualUdpStatus::Success:
    return 0;
  }
  return bsd_support::invalid_argument;
}

} // namespace

bool CompatibilityKernel::send_socket_message(
    Cpu &cpu, std::uint32_t fd, std::uint32_t message_address) {
  const auto socket = virtual_udp_sockets_.find(fd);
  const auto host = host_sockets_.find(fd);
  if (socket == virtual_udp_sockets_.end() && host == host_sockets_.end())
    return false;

  using namespace darwin::socket;
  const auto name_address =
      memory_.read32(message_address + arm32_message::name_offset);
  const auto name_length =
      memory_.read32(message_address + arm32_message::name_length_offset);
  const auto iov_address =
      memory_.read32(message_address + arm32_message::iov_offset);
  const auto iov_count =
      memory_.read32(message_address + arm32_message::iov_count_offset);
  const auto control_address =
      memory_.read32(message_address + arm32_message::control_offset);
  const auto control_length =
      memory_.read32(message_address + arm32_message::control_length_offset);
  if (!name_address || !name_length || !iov_address || !iov_count ||
      !control_address || !control_length) {
    bsd_error(cpu, bsd_support::bad_address);
    return true;
  }
  if (*iov_count > darwin::io::maximum_vector_count ||
      (*iov_count != 0 && *iov_address == 0) ||
      (*name_length != 0 && *name_address == 0) ||
      (*control_length != 0 && *control_address == 0)) {
    bsd_error(cpu, *iov_count > darwin::io::maximum_vector_count
                       ? bsd_support::invalid_argument
                       : bsd_support::bad_address);
    return true;
  }

  std::vector<std::byte> payload;
  for (std::uint32_t index = 0; index < *iov_count; ++index) {
    const auto entry = *iov_address + index * arm32_iovec::size;
    const auto base = memory_.read32(entry + arm32_iovec::base_offset);
    const auto size = memory_.read32(entry + arm32_iovec::length_offset);
    if (!base || !size || (*size != 0 && *base == 0) ||
        *size > bsd_support::maximum_io ||
        payload.size() > bsd_support::maximum_io - *size) {
      bsd_error(cpu, !base || !size || (*size != 0 && *base == 0)
                         ? bsd_support::bad_address
                         : bsd_support::invalid_argument);
      return true;
    }
    if (*size == 0)
      continue;
    const auto bytes = memory_.read_bytes(*base, *size);
    if (!bytes) {
      bsd_error(cpu, bsd_support::bad_address);
      return true;
    }
    payload.insert(payload.end(), bytes->begin(), bytes->end());
  }

  std::vector<std::byte> destination;
  if (*name_length != 0) {
    const auto address = memory_.read_bytes(*name_address, *name_length);
    if (!address) {
      bsd_error(cpu, bsd_support::bad_address);
      return true;
    }
    destination = *address;
  }

  if (host != host_sockets_.end()) {
    const auto result = host->second->send(payload, destination);
    if (result.status == HostSocketStatus::WouldBlock) {
      bsd_error(cpu, bsd_support::would_block);
      return true;
    }
    if (result.status == HostSocketStatus::Error) {
      bsd_error(cpu, result.darwin_error);
      return true;
    }
    bsd_success(cpu, static_cast<std::uint32_t>(result.transferred));
    if (socket_payload_trace_count_ < 32U) {
      output_.write("[network] sendmsg host fd=" + std::to_string(fd) +
                    " bytes=" + std::to_string(result.transferred) + "\n");
      ++socket_payload_trace_count_;
    }
    return true;
  }

  const auto result = destination.empty()
                          ? socket->second->send(payload)
                          : socket->second->send(payload, destination);
  if (result != bsd::VirtualUdpStatus::Success) {
    bsd_error(cpu, socket_error(result));
  } else {
    bsd_success(cpu, static_cast<std::uint32_t>(payload.size()));
    if (socket_payload_trace_count_ < 32U) {
      output_.write("[network] sendmsg virtual UDP fd=" +
                    std::to_string(fd) + " bytes=" +
                    std::to_string(payload.size()) + "\n");
      ++socket_payload_trace_count_;
    }
  }
  return true;
}

} // namespace ilemu
