#include "ilemu/baseband_device.hpp"

#include "ilemu/darwin_tty_abi.hpp"

#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace ilemu::bsd::baseband_device {

namespace {

bool has_numeric_suffix(std::string_view candidate,
                        std::string_view prefix) {
  if (!candidate.starts_with(prefix)) {
    return false;
  }
  const auto suffix = candidate.substr(prefix.size());
  return !suffix.empty() && std::all_of(
      suffix.begin(), suffix.end(), [](const char value) {
        return value >= '0' && value <= '9';
      });
}

} // namespace

bool State::available() const {
  const std::lock_guard lock{mutex_};
  return available_;
}

void State::set_available(bool available) {
  const std::lock_guard lock{mutex_};
  available_ = available;
}

bool State::transmit_queue_writable() const {
  const std::lock_guard lock{mutex_};
  return transmit_queue_writable_;
}

void State::set_transmit_queue_writable(bool writable) {
  const std::lock_guard lock{mutex_};
  transmit_queue_writable_ = writable;
}

bool State::dynamic_channels_available() const {
  const std::lock_guard lock{mutex_};
  return dynamic_channels_available_;
}

void State::set_dynamic_channels_available(bool available) {
  const std::lock_guard lock{mutex_};
  dynamic_channels_available_ = available;
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
  if (transmit_sink_) {
    if (!transmit_sink_(bytes)) {
      return 0;
    }
    return bytes.size();
  }
  if (!transmit_capture_enabled_)
    return bytes.size();
  transmitted_.insert(transmitted_.end(), bytes.begin(), bytes.end());
  return bytes.size();
}

std::vector<std::byte> State::take_transmitted() {
  const std::lock_guard lock{mutex_};
  auto bytes = std::move(transmitted_);
  transmitted_.clear();
  return bytes;
}

void State::set_transmit_capture_enabled(bool enabled) {
  const std::lock_guard lock{mutex_};
  transmit_capture_enabled_ = enabled;
  transmit_sink_ = {};
  if (!enabled)
    transmitted_.clear();
}

void State::set_transmit_sink(TransmitSink sink) {
  const std::lock_guard lock{mutex_};
  transmit_sink_ = std::move(sink);
  transmit_capture_enabled_ = false;
  transmitted_.clear();
}

bool is_mux_channel_path(std::string_view candidate) {
  return has_numeric_suffix(candidate, "/dev/dlci.spi-baseband.") ||
         has_numeric_suffix(candidate, "/dev/dlci.h5.baseband.");
}

bool is_mux_path(std::string_view candidate) {
  return candidate == spi_mux_path || candidate == h5_mux_path ||
         is_mux_channel_path(candidate);
}

bool is_path(std::string_view candidate) {
  return candidate == path || candidate == legacy_path ||
         is_mux_path(candidate);
}

} // namespace ilemu::bsd::baseband_device
