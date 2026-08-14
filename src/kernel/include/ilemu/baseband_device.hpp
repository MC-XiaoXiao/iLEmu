#pragma once

#include <array>
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
// Offline devices with a fixed endpoint retain the finite logical channel
// table expected by stock CommCenter. These channels are not backed by a
// modem and never synthesize receive data.
inline constexpr std::uint32_t offline_mux_channel_capacity = 16;
// In-memory capture is a test/embedding diagnostic, not the production
// transport. Keep it bounded even when a caller explicitly enables it; the
// application uses the streaming sink for unbounded captures.
inline constexpr std::size_t transmit_capture_capacity = 1U * 1024U * 1024U;

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
  // Whether the profile supports guest-visible DLCI setup nodes. Offline
  // devices may expose bounded logical channels without a modem peer.
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
  [[nodiscard]] std::uint32_t modem_control_bits() const;
  void set_modem_control_bits(std::uint32_t bits);
  void update_modem_control_bits(std::uint32_t bits, bool enabled);
  // IOAOS_RECEIVE_QUEUE installs a fixed-size driver queue descriptor. The
  // offline endpoint records the bounded descriptor for state inspection but
  // never dereferences guest pointers or creates synthetic receive bytes.
  [[nodiscard]] bool configure_receive_queue(
      std::span<const std::byte> configuration);
  [[nodiscard]] bool receive_queue_configured() const;
  // TIOCFLUSH clears the software receive queue. Writes are synchronous, so
  // there is no pending transmit queue to retain or fabricate.
  void flush_buffers(std::uint32_t what);
  // A zero capacity preserves the virtual/replay transport's dynamic channel
  // allocation. Offline transport bounds both its anonymous slots and its
  // named logical-channel registry; named IDs remain outside the anonymous
  // slot range.
  void set_mux_channel_capacity(std::uint32_t capacity);
  [[nodiscard]] std::uint32_t register_mux_channel(std::string_view name);
  [[nodiscard]] std::optional<std::uint32_t>
  mux_channel(std::string_view name) const;
  void enqueue_receive(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> receive(std::size_t maximum);
  [[nodiscard]] std::size_t pending_receive_bytes() const;
  [[nodiscard]] std::size_t write(std::span<const std::byte> bytes);
  [[nodiscard]] std::vector<std::byte> take_transmitted();
  // Capture is opt-in. The application installs either a null sink or a
  // streaming sink, and tests/embedding callers explicitly enable bounded
  // in-memory capture when they need to inspect recent bytes.
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
  std::uint32_t modem_control_bits_{};
  std::array<std::byte, darwin::tty::receive_queue_configuration_size>
      receive_queue_configuration_{};
  bool receive_queue_configured_{};
  std::uint32_t anonymous_mux_channel_capacity_{};
  std::uint32_t next_anonymous_mux_channel_{1};
  std::map<std::string, std::uint32_t> mux_channels_;
  std::uint32_t next_mux_channel_{1};
  darwin::tty::Arm32Attributes attributes_{darwin::tty::default_attributes()};
  std::deque<std::byte> receive_queue_;
  std::vector<std::byte> transmitted_;
  TransmitSink transmit_sink_;
  bool transmit_capture_enabled_{};
};

[[nodiscard]] bool is_path(std::string_view candidate);
[[nodiscard]] bool is_mux_channel_path(std::string_view candidate);
[[nodiscard]] bool is_mux_path(std::string_view candidate);

} // namespace ilemu::bsd::baseband_device
