#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace ilemu {

struct ContentIdentity {
  std::array<std::byte, 32> digest{};

  friend constexpr bool operator==(const ContentIdentity &,
                                   const ContentIdentity &) = default;
  friend constexpr auto operator<=>(const ContentIdentity &,
                                    const ContentIdentity &) = default;

  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] std::string hex() const;
};

struct ContentIdentityHash {
  [[nodiscard]] std::size_t operator()(
      const ContentIdentity &identity) const noexcept;
};

[[nodiscard]] ContentIdentity sha256(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<ContentIdentity> sha256_file(
    const std::filesystem::path &path, std::uint64_t file_offset = 0,
    std::optional<std::uint64_t> byte_count = std::nullopt);

} // namespace ilemu
