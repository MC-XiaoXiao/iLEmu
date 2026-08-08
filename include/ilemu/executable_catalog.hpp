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
#include "ilemu/dyld_shared_cache.hpp"
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
  std::uint64_t file_size{};
  bool fat_container{};
  std::vector<std::string> dependencies;
  std::vector<ExecutableMappingIdentity> mappings;
};

struct ExecutableCatalogScanSummary {
  std::size_t regular_files{};
  std::size_t mach_o_images{};
  std::size_t dyld_shared_cache_generations{};
  std::size_t dyld_shared_cache_images{};
  std::size_t failed_files{};
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
  [[nodiscard]] std::size_t register_shared_cache(
      const DyldSharedCache &cache);
  // Scans a firmware or overlay tree without treating unrelated data files as
  // executable input. Malformed candidates are counted and skipped so an
  // incomplete optional bundle cannot prevent a catalog rebuild.
  [[nodiscard]] ExecutableCatalogScanSummary register_tree(
      const std::filesystem::path &root,
      ArmArchitectureVersion architecture = ArmArchitectureVersion::Armv6K);

  // The manifest is versioned and written through a temporary file followed
  // by rename. Malformed input is rejected without changing the live index so
  // callers can safely rebuild it from the firmware tree.
  [[nodiscard]] bool load(const std::filesystem::path &path) noexcept;
  [[nodiscard]] bool save(const std::filesystem::path &path) const noexcept;

  [[nodiscard]] const ExecutableCatalogEntry *find(
      const ContentIdentity &identity) const;
  [[nodiscard]] const ExecutableCatalogEntry *find_path(
      const std::filesystem::path &path) const;
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
