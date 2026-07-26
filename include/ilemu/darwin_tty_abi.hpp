#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ilemu::darwin::tty {

// XNU 792 bsd/sys/ioccom.h and bsd/sys/ttycom.h. Darwin encodes a
// parameter-less ioctl as IOC_VOID | group << 8 | command.
inline constexpr std::uint32_t ioctl_void = 0x2000'0000U;
inline constexpr std::uint32_t ioctl_output = 0x4000'0000U;
inline constexpr std::uint32_t ioctl_input = 0x8000'0000U;
inline constexpr std::uint32_t parameter_length_mask = 0x1fffU;

constexpr std::uint32_t void_command(char group, std::uint8_t command) {
  return ioctl_void |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(group)) << 8U) |
         command;
}

constexpr std::uint32_t sized_command(std::uint32_t direction, char group,
                                      std::uint8_t command,
                                      std::uint32_t size) {
  return direction | ((size & parameter_length_mask) << 16U) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(group)) << 8U) |
         command;
}

constexpr std::uint32_t parameter_length(std::uint32_t command) {
  return (command >> 16U) & parameter_length_mask;
}

inline constexpr std::size_t control_character_count = 20;
inline constexpr std::size_t minimum_bytes_index = 16; // VMIN
inline constexpr std::size_t timeout_deciseconds_index = 17; // VTIME
inline constexpr std::uint32_t arm32_attributes_size = 44;

namespace arm32_attributes_offset {
inline constexpr std::uint32_t input_flags = 0;
inline constexpr std::uint32_t output_flags = 4;
inline constexpr std::uint32_t control_flags = 8;
inline constexpr std::uint32_t local_flags = 12;
inline constexpr std::uint32_t control_characters = 16;
inline constexpr std::uint32_t input_speed = 36;
inline constexpr std::uint32_t output_speed = 40;
} // namespace arm32_attributes_offset

struct Arm32Attributes {
  std::uint32_t input_flags{};
  std::uint32_t output_flags{};
  std::uint32_t control_flags{};
  std::uint32_t local_flags{};
  std::array<std::uint8_t, control_character_count> control_characters{};
  std::int32_t input_speed{};
  std::int32_t output_speed{};
};

constexpr Arm32Attributes default_attributes() {
  // XNU 792 ttydefaults.h: TTYDEF_{I,O,C,L}FLAG, ttydefchars and B9600.
  return {
      .input_flags = 0x0000'2b02U,
      .output_flags = 0x0000'0003U,
      .control_flags = 0x0000'4b00U,
      .local_flags = 0x0000'05cbU,
      .control_characters = {4,  255, 255, 127, 23, 21, 18, 255, 3,  28,
                             26, 25,  17,  19,  22, 15, 1,  0,   20, 255},
      .input_speed = 9'600,
      .output_speed = 9'600,
  };
}

inline constexpr std::uint32_t set_exclusive = void_command('t', 13);
inline constexpr std::uint32_t clear_exclusive = void_command('t', 14);
inline constexpr std::uint32_t get_attributes =
    sized_command(ioctl_output, 't', 19, arm32_attributes_size);
inline constexpr std::uint32_t set_attributes =
    sized_command(ioctl_input, 't', 20, arm32_attributes_size);
inline constexpr std::uint32_t set_attributes_after_drain =
    sized_command(ioctl_input, 't', 21, arm32_attributes_size);
inline constexpr std::uint32_t set_attributes_after_drain_and_flush =
    sized_command(ioctl_input, 't', 22, arm32_attributes_size);
inline constexpr std::uint32_t drain_output = void_command('t', 94);
inline constexpr std::uint32_t set_controlling_terminal =
    void_command('t', 97);

// AppleIOSerialFamily accepts arbitrary speeds through IOSSIOSPEED instead of
// encoding every rate in termios. speed_t is 32-bit in the ARM guest ABI.
inline constexpr std::uint32_t set_arbitrary_speed =
    sized_command(ioctl_input, 'T', 2, sizeof(std::uint32_t));

// Apple Onboard Serial driver contract used by the target CommCenter.
// The firmware names this request IOAOSH5 and passes a 32-bit boolean that
// selects the H5 framed transport.
inline constexpr std::uint32_t set_h5_transport_mode =
    sized_command(ioctl_input, 'T', 10, sizeof(std::uint32_t));

} // namespace ilemu::darwin::tty
