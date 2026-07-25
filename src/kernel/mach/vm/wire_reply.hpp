#pragma once

#include "ilemu/address_space.hpp"
#include "ilemu/mig_wire_abi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ilemu::mach_vm_support {

inline constexpr std::uint32_t kern_success = 0;
inline constexpr std::uint32_t kern_invalid_address = 1;
inline constexpr std::uint32_t kern_resource_shortage = 6;
inline constexpr std::uint32_t mach_receive_invalid_data = 0x1000'4008U;
inline constexpr std::uint32_t simple_reply_size =
    darwin::mig_wire::simple_reply_payload_base;

inline bool write_words(AddressSpace &memory, std::uint32_t address,
                        std::span<const std::uint32_t> words) {
  for (std::size_t index = 0; index < words.size(); ++index) {
    if (!memory.write32(address + static_cast<std::uint32_t>(index) *
                                      darwin::mig_wire::word_size,
                        words[index])) {
      return false;
    }
  }
  return true;
}

inline bool write_simple_reply(AddressSpace &memory, std::uint32_t address,
                               std::uint32_t reply_port,
                               std::uint32_t identifier, std::uint32_t result) {
  const std::array reply{
      darwin::mig_wire::message_bits(
          darwin::mig_wire::disposition_move_send_once),
      simple_reply_size,
      reply_port,
      0U,
      0U,
      identifier + 100U,
      0U, // NDR migration/character/float representation word
      1U, // NDR little-endian integer representation word
      result,
  };
  return write_words(memory, address, reply);
}

} // namespace ilemu::mach_vm_support
