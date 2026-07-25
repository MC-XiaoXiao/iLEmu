#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

#include "ilemu/darwin_tty_abi.hpp"

namespace ilemu::bsd::baseband_device {

inline constexpr std::string_view path{"/dev/h5.baseband"};
inline constexpr std::string_view directory_name{"h5.baseband"};
inline constexpr std::string_view descriptor_kind{"baseband"};
inline constexpr unsigned device_minor = 3;

enum class IoctlResult {
  success,
  unsupported,
};

class State {
public:
  [[nodiscard]] bool may_open(bool privileged) const;
  [[nodiscard]] IoctlResult ioctl(std::uint32_t command);
  [[nodiscard]] bool exclusive() const;
  [[nodiscard]] darwin::tty::Arm32Attributes attributes() const;
  void set_attributes(const darwin::tty::Arm32Attributes &attributes);
  [[nodiscard]] bool h5_transport_mode() const;
  void set_h5_transport_mode(bool enabled);
  void enqueue_receive(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> receive(std::size_t maximum);
  [[nodiscard]] std::size_t pending_receive_bytes() const;
  [[nodiscard]] std::size_t write(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> take_transmitted();

private:
  mutable std::mutex mutex_;
  bool exclusive_{};
  bool h5_transport_mode_{};
  darwin::tty::Arm32Attributes attributes_{darwin::tty::default_attributes()};
  std::deque<std::byte> receive_queue_;
  std::vector<std::byte> transmitted_;
};

[[nodiscard]] bool is_path(std::string_view candidate);

} // namespace ilemu::bsd::baseband_device
