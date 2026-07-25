#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ilemu::mach_transport {

enum class DescriptorKind : std::uint8_t {
  Port,
  OutOfLineMemory,
  OutOfLinePorts,
};

struct Descriptor {
  DescriptorKind kind{DescriptorKind::Port};
  std::uint32_t offset{};
  std::uint32_t address_or_name{};
  std::uint32_t count_or_size{};
  std::uint32_t metadata{};

  [[nodiscard]] bool deallocate() const;
  [[nodiscard]] std::uint32_t disposition() const;
};

// Parses the natural-aligned 32-bit descriptor table used by Darwin 8. An
// empty vector is a valid simple message; nullopt denotes malformed or unknown
// complex descriptor data.
[[nodiscard]] std::optional<std::vector<Descriptor>>
parse_descriptors(std::span<const std::byte> message);

} // namespace ilemu::mach_transport
