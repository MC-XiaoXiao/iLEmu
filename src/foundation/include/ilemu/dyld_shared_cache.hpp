#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ilemu/content_identity.hpp"
#include "ilemu/file_page_cache.hpp"
#include "ilemu/arm_cpu_model.hpp"

namespace ilemu {

class MachOImage;

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
  std::optional<GuestFileGeneration> file_generation;
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
  static constexpr std::uint32_t hle_profile_schema_version = 1;

  // Pointer-like parse result retaining the historical optional-style
  // has_value()/operator* API while allowing all successful callers to share
  // one immutable generation object.
  class ParseResult {
  public:
    ParseResult() = default;
    ParseResult(std::shared_ptr<const DyldSharedCache> generation) noexcept;

    [[nodiscard]] bool has_value() const noexcept;
    explicit operator bool() const noexcept;
    [[nodiscard]] const DyldSharedCache *operator->() const noexcept;
    [[nodiscard]] const DyldSharedCache &operator*() const noexcept;
    [[nodiscard]] std::shared_ptr<const DyldSharedCache> shared() const noexcept;

  private:
    std::shared_ptr<const DyldSharedCache> generation_;
  };

  DyldSharedCache();
  ~DyldSharedCache();
  DyldSharedCache(const DyldSharedCache &) = delete;
  DyldSharedCache &operator=(const DyldSharedCache &) = delete;
  DyldSharedCache(DyldSharedCache &&) noexcept;
  DyldSharedCache &operator=(DyldSharedCache &&) noexcept;

  [[nodiscard]] static ParseResult
  parse(const std::filesystem::path &path,
        DyldSharedCacheOptions options = {});

  struct ParseStats {
    std::uint64_t generation_builds{};
    std::uint64_t generation_hits{};
    std::uint64_t image_builds{};
    std::uint64_t image_hits{};
  };

  [[nodiscard]] static ParseStats parse_stats() noexcept;

  // Returns one immutable parsed Mach-O view per logical image and guest
  // architecture. The returned object is shared by all registries/processes
  // that use this cache generation.
  [[nodiscard]] std::shared_ptr<const MachOImage> parse_image(
      std::uint32_t image_index,
      ArmArchitectureVersion architecture) const;
  [[nodiscard]] std::vector<std::uint32_t>
  images_intersecting_file_range(std::uint32_t file_index,
                                 std::uint64_t file_offset,
                                 std::uint64_t size) const;

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
  struct ImageStore;
  struct ImageRangeIndexEntry {
    std::uint64_t file_offset{};
    std::uint64_t file_end{};
    std::uint64_t prefix_file_end{};
    std::uint32_t image_index{};
  };

  DyldCacheFile main_cache_;
  std::vector<DyldCacheFile> files_;
  std::vector<DyldCacheImage> images_;
  ContentIdentity generation_identity_;
  std::uint32_t platform_{};
  std::uint8_t format_version_{};
  std::uint64_t shared_region_start_{};
  std::uint64_t shared_region_size_{};
  std::uint64_t max_slide_{};
  std::vector<std::vector<ImageRangeIndexEntry>> image_range_index_;
  std::shared_ptr<ImageStore> image_store_;
};

} // namespace ilemu
