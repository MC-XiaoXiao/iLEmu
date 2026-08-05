#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ilemu/darwin_tty_abi.hpp"

namespace ilemu::bsd::baseband_device {

inline constexpr std::string_view path{"/dev/h5.baseband"};
inline constexpr std::string_view spi_mux_path{"/dev/mux.spi-baseband"};
inline constexpr std::string_view h5_mux_path{"/dev/mux.h5.baseband"};
inline constexpr std::string_view directory_name{"h5.baseband"};
inline constexpr std::string_view spi_mux_directory_name{"mux.spi-baseband"};
inline constexpr std::string_view h5_mux_directory_name{"mux.h5.baseband"};
inline constexpr std::string_view descriptor_kind{"baseband"};
inline constexpr unsigned device_minor = 3;

enum class IoctlResult {
  success,
  unsupported,
};

class State {
public:
  [[nodiscard]] bool available() const;
  void set_available(bool available);
  [[nodiscard]] bool may_open(bool privileged) const;
  [[nodiscard]] IoctlResult ioctl(std::uint32_t command);
  [[nodiscard]] bool exclusive() const;
  [[nodiscard]] darwin::tty::Arm32Attributes attributes() const;
  void set_attributes(const darwin::tty::Arm32Attributes &attributes);
  [[nodiscard]] bool h5_transport_mode() const;
  void set_h5_transport_mode(bool enabled);
  [[nodiscard]] std::size_t minimum_receive_bytes() const;
  void set_minimum_receive_bytes(std::size_t bytes);
  [[nodiscard]] std::uint32_t register_mux_channel(std::string_view name);
  [[nodiscard]] std::optional<std::uint32_t>
  mux_channel(std::string_view name) const;
  void enqueue_receive(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> receive(std::size_t maximum);
  [[nodiscard]] std::size_t pending_receive_bytes() const;
  [[nodiscard]] std::size_t write(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> take_transmitted();

private:
  mutable std::mutex mutex_;
  bool available_{true};
  bool exclusive_{};
  bool h5_transport_mode_{};
  std::size_t minimum_receive_bytes_{};
  std::map<std::string, std::uint32_t> mux_channels_;
  std::uint32_t next_mux_channel_{1};
  darwin::tty::Arm32Attributes attributes_{darwin::tty::default_attributes()};
  std::deque<std::byte> receive_queue_;
  std::vector<std::byte> transmitted_;
};

[[nodiscard]] bool is_path(std::string_view candidate);

} // namespace ilemu::bsd::baseband_device
