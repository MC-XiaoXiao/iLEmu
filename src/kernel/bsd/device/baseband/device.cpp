#include "ilemu/baseband_device.hpp"

#include "ilemu/darwin_tty_abi.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace ilemu::bsd::baseband_device {

bool State::available() const {
  const std::lock_guard lock{mutex_};
  return available_;
}

void State::set_available(bool available) {
  const std::lock_guard lock{mutex_};
  available_ = available;
}

bool State::transport_writable() const {
  const std::lock_guard lock{mutex_};
  return transport_writable_;
}

void State::set_transport_writable(bool writable) {
  const std::lock_guard lock{mutex_};
  transport_writable_ = writable;
}

bool State::may_open(bool privileged) const {
  const std::lock_guard lock{mutex_};
  return available_ && (privileged || !exclusive_);
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

std::size_t State::minimum_receive_bytes() const {
  const std::lock_guard lock{mutex_};
  return minimum_receive_bytes_;
}

void State::set_minimum_receive_bytes(std::size_t bytes) {
  const std::lock_guard lock{mutex_};
  minimum_receive_bytes_ = bytes;
}

void State::set_mux_channel_capacity(std::uint32_t capacity) {
  const std::lock_guard lock{mutex_};
  anonymous_mux_channel_capacity_ = capacity;
  next_anonymous_mux_channel_ = 1;
  // Keep named channels out of the anonymous slot range.  The normal boot
  // configures this before CommCenter opens the device, so changing a live
  // transport remains a safe administrative operation as well.
  if (capacity != 0 && next_mux_channel_ <= capacity) {
    next_mux_channel_ = capacity + 1U;
  }
}

std::uint32_t State::register_mux_channel(std::string_view name) {
  const std::lock_guard lock{mutex_};
  if (!name.empty()) {
    const auto key = std::string{name};
    if (const auto found = mux_channels_.find(key); found != mux_channels_.end()) {
      return found->second;
    }
    const auto unit = next_mux_channel_++;
    mux_channels_.emplace(key, unit);
    return unit;
  }
  if (anonymous_mux_channel_capacity_ != 0) {
    const auto unit = next_anonymous_mux_channel_;
    next_anonymous_mux_channel_ =
        unit == anonymous_mux_channel_capacity_ ? 1U : unit + 1U;
    return unit;
  }
  return next_mux_channel_++;
}

std::optional<std::uint32_t> State::mux_channel(std::string_view name) const {
  const std::lock_guard lock{mutex_};
  if (name.empty()) {
    return std::nullopt;
  }
  const auto found = mux_channels_.find(std::string{name});
  if (found == mux_channels_.end()) {
    return std::nullopt;
  }
  return found->second;
}

void State::enqueue_receive(std::span<const std::byte> bytes) {
  const std::lock_guard lock{mutex_};
  receive_queue_.insert(receive_queue_.end(), bytes.begin(), bytes.end());
}

std::vector<std::byte> State::receive(std::size_t maximum) {
  const std::lock_guard lock{mutex_};
  if (minimum_receive_bytes_ != 0 &&
      receive_queue_.size() < minimum_receive_bytes_) {
    return {};
  }
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
  if (minimum_receive_bytes_ != 0 &&
      receive_queue_.size() < minimum_receive_bytes_) {
    return 0;
  }
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

bool is_path(std::string_view candidate) {
  return candidate == path || candidate == spi_mux_path ||
         candidate == h5_mux_path;
}

} // namespace ilemu::bsd::baseband_device
