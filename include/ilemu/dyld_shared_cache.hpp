#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ilemu/content_identity.hpp"

namespace ilemu {

using DyldCacheUuid = std::array<std::byte, 16>;

struct DyldCacheRange {
  std::uint64_t address{};
  std::uint64_t size{};
  std::uint64_t file_offset{};
  std::uint32_t file_index{};
  std::uint32_t initial_protection{};
  std::uint32_t maximum_protection{};
};

struct DyldCacheMapping : DyldCacheRange {
  std::uint64_t slide_info_offset{};
  std::uint64_t slide_info_size{};
  std::uint64_t flags{};
  std::optional<std::uint32_t> slide_info_version;
};

struct DyldCacheImage {
  std::uint32_t index{};
  std::string path;
  std::uint64_t unslid_load_address{};
  std::optional<DyldCacheUuid> text_uuid;
  std::uint64_t text_segment_size{};
  std::vector<DyldCacheRange> executable_ranges;
  std::optional<ContentIdentity> text_identity;
};

struct DyldCacheFile {
  std::filesystem::path path;
  std::uintmax_t file_size{};
  ContentIdentity content_identity;
  DyldCacheUuid uuid{};
  std::uint64_t cache_vm_offset{};
  std::string file_suffix;
  std::vector<DyldCacheMapping> mappings;
};

struct DyldSharedCacheOptions {
  // When empty, the 16-byte magic is used as the architecture identity in
  // the generation key. A firmware catalog can provide a normalized Guest
  // ISA tag instead.
  std::string architecture;
  // Entries are ordered exactly as the main cache's subCacheArray. If this is
  // empty, fileSuffix is used to discover each sibling next to the main file.
  std::vector<std::filesystem::path> subcache_paths;
};

class DyldSharedCache {
public:
  static constexpr std::uint32_t parser_schema_version = 1;

  [[nodiscard]] static std::optional<DyldSharedCache>
  parse(const std::filesystem::path &path,
        DyldSharedCacheOptions options = {});

  [[nodiscard]] const DyldCacheFile &main_cache() const noexcept;
  [[nodiscard]] std::span<const DyldCacheFile> files() const noexcept;
  [[nodiscard]] std::span<const DyldCacheImage> images() const noexcept;
  [[nodiscard]] const ContentIdentity &generation_identity() const noexcept;
  [[nodiscard]] std::uint32_t platform() const noexcept;
  [[nodiscard]] std::uint8_t format_version() const noexcept;
  [[nodiscard]] std::uint64_t shared_region_start() const noexcept;
  [[nodiscard]] std::uint64_t shared_region_size() const noexcept;
  [[nodiscard]] std::uint64_t max_slide() const noexcept;

  [[nodiscard]] const DyldCacheImage *find_image(
      std::string_view install_name) const noexcept;

private:
  DyldCacheFile main_cache_;
  std::vector<DyldCacheFile> files_;
  std::vector<DyldCacheImage> images_;
  ContentIdentity generation_identity_;
  std::uint32_t platform_{};
  std::uint8_t format_version_{};
  std::uint64_t shared_region_start_{};
  std::uint64_t shared_region_size_{};
  std::uint64_t max_slide_{};
};

} // namespace ilemu
