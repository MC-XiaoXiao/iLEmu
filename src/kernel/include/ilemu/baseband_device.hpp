#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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

enum class IoctlResult {
  success,
  unsupported,
};

class State {
public:
  using TransmitSink =
      std::function<bool(std::span<const std::byte>)>;

  [[nodiscard]] bool available() const;
  void set_available(bool available);
  // This is the guest-to-device tty transmit queue, not modem availability.
  [[nodiscard]] bool transmit_queue_writable() const;
  void set_transmit_queue_writable(bool writable);
  // Dynamic DLCI nodes require a modem-side mux endpoint.
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
  // allocation. Offline transport rejects mux nodes before channel
  // allocation, so no logical DLCI is synthesized.
  void set_mux_channel_capacity(std::uint32_t capacity);
  [[nodiscard]] std::uint32_t register_mux_channel(std::string_view name);
  [[nodiscard]] std::optional<std::uint32_t>
  mux_channel(std::string_view name) const;
  void enqueue_receive(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> receive(std::size_t maximum);
  [[nodiscard]] std::size_t pending_receive_bytes() const;
  [[nodiscard]] std::size_t write(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> take_transmitted();
  // The application normally installs a null sink. Tests and embedding
  // callers retain the legacy in-memory capture until they opt out.
  void set_transmit_capture_enabled(bool enabled);
  // A sink receives one complete guest write while the device state is
  // serialized. Returning false makes that write fail instead of reporting a
  // partial fixed-node write.
  void set_transmit_sink(TransmitSink sink);

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
  TransmitSink transmit_sink_;
  bool transmit_capture_enabled_{true};
};

[[nodiscard]] bool is_path(std::string_view candidate);
[[nodiscard]] bool is_mux_channel_path(std::string_view candidate);
[[nodiscard]] bool is_mux_path(std::string_view candidate);

} // namespace ilemu::bsd::baseband_device
