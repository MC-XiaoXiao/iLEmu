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
// Darwin 9/early iPhoneOS CommCenter probes the legacy serial node before
// selecting the H5 transport. It is the same emulated baseband endpoint, not
// a second transport or a firmware-specific success result.
inline constexpr std::string_view legacy_path{"/dev/cu.baseband"};
inline constexpr std::string_view spi_mux_path{"/dev/mux.spi-baseband"};
inline constexpr std::string_view h5_mux_path{"/dev/mux.h5.baseband"};
inline constexpr std::string_view directory_name{"h5.baseband"};
inline constexpr std::string_view spi_mux_directory_name{"mux.spi-baseband"};
inline constexpr std::string_view h5_mux_directory_name{"mux.h5.baseband"};
inline constexpr std::string_view descriptor_kind{"baseband"};
inline constexpr unsigned device_minor = 3;
// The no-modem profile still exposes the serial-mux ABI used by stock
// CommCenter.  The client keeps a finite channel table; bounding anonymous
// allocations prevents a silent transport from turning retries into an
// ever-growing logical channel id stream.
inline constexpr std::uint32_t offline_mux_channel_capacity = 16;

enum class IoctlResult {
  success,
  unsupported,
};

class State {
public:
  [[nodiscard]] bool available() const;
  void set_available(bool available);
  // This is the guest-to-device tty transmit queue, not modem availability.
  // An offline device can still consume a complete command without producing
  // a reply, just as a serial driver can drain bytes after its peer disappears.
  [[nodiscard]] bool transmit_queue_writable() const;
  void set_transmit_queue_writable(bool writable);
  // Dynamic DLCI nodes require a modem-side mux endpoint.  Keep this separate
  // from the fixed tty's transmit queue so an offline profile can accept a
  // complete command while continuing to reject unsupported channels.
  [[nodiscard]] bool dynamic_channels_available() const;
  void set_dynamic_channels_available(bool available);
  [[nodiscard]] bool may_open(bool privileged) const;
  [[nodiscard]] IoctlResult ioctl(std::uint32_t command);
  [[nodiscard]] bool exclusive() const;
  [[nodiscard]] darwin::tty::Arm32Attributes attributes() const;
  void set_attributes(const darwin::tty::Arm32Attributes &attributes);
  [[nodiscard]] bool h5_transport_mode() const;
  void set_h5_transport_mode(bool enabled);
  [[nodiscard]] std::size_t minimum_receive_bytes() const;
  void set_minimum_receive_bytes(std::size_t bytes);
  // A zero capacity preserves the virtual/replay transport's dynamic channel
  // allocation.  Offline transport uses the stock client's finite channel
  // table and reuses anonymous slots when CommCenter retries setup.
  void set_mux_channel_capacity(std::uint32_t capacity);
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
  bool transmit_queue_writable_{true};
  bool dynamic_channels_available_{true};
  bool exclusive_{};
  bool h5_transport_mode_{};
  std::size_t minimum_receive_bytes_{};
  std::uint32_t anonymous_mux_channel_capacity_{};
  std::uint32_t next_anonymous_mux_channel_{1};
  std::map<std::string, std::uint32_t> mux_channels_;
  std::uint32_t next_mux_channel_{1};
  darwin::tty::Arm32Attributes attributes_{darwin::tty::default_attributes()};
  std::deque<std::byte> receive_queue_;
  std::vector<std::byte> transmitted_;
};

[[nodiscard]] bool is_path(std::string_view candidate);
[[nodiscard]] bool is_mux_channel_path(std::string_view candidate);

} // namespace ilemu::bsd::baseband_device
