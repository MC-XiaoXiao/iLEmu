#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "ilemu/content_identity.hpp"
#include "ilemu/macho.hpp"

namespace ilemu {

enum class ExecutableCatalogKind : std::uint8_t {
  MachO,
  Dylib,
  Framework,
  PlugIn,
  DynamicMapping,
};

struct ExecutableMappingIdentity {
  std::uint64_t file_offset{};
  std::uint64_t byte_count{};

  friend constexpr bool operator==(const ExecutableMappingIdentity &,
                                   const ExecutableMappingIdentity &) =
      default;
};

struct ExecutableCatalogEntry {
  ContentIdentity content_identity;
  std::vector<std::filesystem::path> aliases;
  std::set<ExecutableCatalogKind> kinds;
  std::optional<std::array<std::byte, 16>> uuid;
  std::uint32_t cpu_type{};
  std::uint32_t cpu_subtype{};
  std::uint32_t file_type{};
  bool fat_container{};
  std::vector<std::string> dependencies;
  std::vector<ExecutableMappingIdentity> mappings;
};

class ExecutableCatalog {
public:
  [[nodiscard]] const ExecutableCatalogEntry &register_image(
      const MachOImage &image);
  [[nodiscard]] const ExecutableCatalogEntry &register_path(
      const std::filesystem::path &path,
      ArmArchitectureVersion architecture = ArmArchitectureVersion::Armv6K);
  [[nodiscard]] const ExecutableCatalogEntry &register_mapping(
      const std::filesystem::path &path, std::uint64_t file_offset,
      std::uint64_t byte_count);

  [[nodiscard]] const ExecutableCatalogEntry *find(
      const ContentIdentity &identity) const;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
  [[nodiscard]] static std::filesystem::path normalize_path(
      const std::filesystem::path &path);
  [[nodiscard]] static ExecutableCatalogKind classify(
      const MachOImage &image, const std::filesystem::path &path);
  [[nodiscard]] ExecutableCatalogEntry &upsert(
      ContentIdentity identity, const std::filesystem::path &path,
      ExecutableCatalogKind kind);

  std::vector<ExecutableCatalogEntry> entries_;
  std::unordered_map<ContentIdentity, std::size_t, ContentIdentityHash>
      identity_index_;
};

} // namespace ilemu
