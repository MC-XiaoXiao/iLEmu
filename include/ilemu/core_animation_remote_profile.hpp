#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ilemu {

class MachOImage;

// Describes the private CoreAnimation transaction messages sent from a
// client context to the firmware window server. Selection follows the
// encoder implementation exported by the loaded QuartzCore image; it does
// not depend on an OS build, application, or page.
struct CoreAnimationRemoteProfile {
  std::string_view name;
  std::uint32_t inline_transaction_message{};
  std::uint32_t out_of_line_transaction_message{};
  // Some early UIKit builds submit their transaction through a Thumb-2
  // encoder whose message opcode is not materialized as an adjacent literal
  // arithmetic sequence. Their exported render-server protocol still gives
  // us a stable, firmware-owned rendezvous point.
  bool render_server_port_rendezvous{};

  [[nodiscard]] bool
  is_transaction_message(std::uint32_t identifier) const;

  [[nodiscard]] static std::optional<CoreAnimationRemoteProfile>
  detect(const MachOImage &image);

  bool operator==(const CoreAnimationRemoteProfile &) const = default;
};

} // namespace ilemu
