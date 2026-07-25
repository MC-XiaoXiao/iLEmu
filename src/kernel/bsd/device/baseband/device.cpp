#include "ilemu/baseband_device.hpp"

#include "ilemu/darwin_tty_abi.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace ilemu::bsd::baseband_device {

bool State::may_open(bool privileged) const {
  const std::lock_guard lock{mutex_};
  return privileged || !exclusive_;
}

IoctlResult State::ioctl(std::uint32_t command) {
  const std::lock_guard lock{mutex_};
  switch (command) {
  case darwin::tty::set_exclusive:
    exclusive_ = true;
    return IoctlResult::success;
  case darwin::tty::clear_exclusive:
    exclusive_ = false;
    return IoctlResult::success;
  default:
    return IoctlResult::unsupported;
  }
}

bool State::exclusive() const {
  const std::lock_guard lock{mutex_};
  return exclusive_;
}

darwin::tty::Arm32Attributes State::attributes() const {
  const std::lock_guard lock{mutex_};
  return attributes_;
}

void State::set_attributes(const darwin::tty::Arm32Attributes &attributes) {
  const std::lock_guard lock{mutex_};
  attributes_ = attributes;
}

bool State::h5_transport_mode() const {
  const std::lock_guard lock{mutex_};
  return h5_transport_mode_;
}

void State::set_h5_transport_mode(bool enabled) {
  const std::lock_guard lock{mutex_};
  h5_transport_mode_ = enabled;
}

void State::enqueue_receive(std::span<const std::byte> bytes) {
  const std::lock_guard lock{mutex_};
  receive_queue_.insert(receive_queue_.end(), bytes.begin(), bytes.end());
}

std::vector<std::byte> State::receive(std::size_t maximum) {
  const std::lock_guard lock{mutex_};
  const auto count = std::min(maximum, receive_queue_.size());
  std::vector<std::byte> bytes;
  bytes.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    bytes.push_back(receive_queue_.front());
    receive_queue_.pop_front();
  }
  return bytes;
}

std::size_t State::pending_receive_bytes() const {
  const std::lock_guard lock{mutex_};
  return receive_queue_.size();
}

std::size_t State::write(std::span<const std::byte> bytes) {
  const std::lock_guard lock{mutex_};
  transmitted_.insert(transmitted_.end(), bytes.begin(), bytes.end());
  return bytes.size();
}

std::vector<std::byte> State::take_transmitted() {
  const std::lock_guard lock{mutex_};
  auto bytes = std::move(transmitted_);
  transmitted_.clear();
  return bytes;
}

bool is_path(std::string_view candidate) { return candidate == path; }

} // namespace ilemu::bsd::baseband_device
