#include "ilemu/dyld_shared_cache.hpp"
#include "ilemu/macho.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ilemu {

struct DyldSharedCache::ImageStore {
  std::mutex mutex;
  std::map<std::pair<std::uint32_t, std::uint8_t>,
           std::shared_ptr<const MachOImage>> images;
};

struct DyldSharedCache::GenerationArtifactView {
  static std::shared_ptr<const GenerationArtifactView>
  open(const std::filesystem::path &path,
       std::size_t maximum_size = 256U * 1024U * 1024U) {
    const auto descriptor =
        ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) return {};
    struct stat artifact_stat {};
    if (::fstat(descriptor, &artifact_stat) != 0 ||
        !S_ISREG(artifact_stat.st_mode) ||
        (artifact_stat.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) != 0 ||
        artifact_stat.st_size <= 0 ||
        static_cast<std::uint64_t>(artifact_stat.st_size) > maximum_size) {
      static_cast<void>(::close(descriptor));
      return {};
    }
    const auto byte_size = static_cast<std::size_t>(artifact_stat.st_size);
    void *mapping =
        ::mmap(nullptr, byte_size, PROT_READ, MAP_SHARED, descriptor, 0);
    if (mapping == MAP_FAILED) {
      static_cast<void>(::close(descriptor));
      return {};
    }

    const auto lease_path = std::filesystem::path{
        path.string() + ".lease-" + std::to_string(::getpid()) + "-" +
        std::to_string(next_lease_identity.fetch_add(
            1, std::memory_order_relaxed))};
    const auto lease_descriptor = ::open(
        lease_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (lease_descriptor < 0) {
      static_cast<void>(::munmap(mapping, byte_size));
      static_cast<void>(::close(descriptor));
      return {};
    }
    static_cast<void>(::close(lease_descriptor));
    return std::shared_ptr<const GenerationArtifactView>{
        new GenerationArtifactView{descriptor, mapping, byte_size,
                                   lease_path}};
  }

  ~GenerationArtifactView() {
    if (mapping != nullptr && byte_size != 0U)
      static_cast<void>(::munmap(mapping, byte_size));
    if (descriptor >= 0) static_cast<void>(::close(descriptor));
    if (!lease_path.empty()) {
      std::error_code error;
      std::filesystem::remove(lease_path, error);
    }
  }

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return {reinterpret_cast<const std::byte *>(mapping), byte_size};
  }

private:
  GenerationArtifactView(int descriptor_value, void *mapping_value,
                         std::size_t byte_size_value,
                         std::filesystem::path lease_path_value) noexcept
      : descriptor{descriptor_value}, mapping{mapping_value},
        byte_size{byte_size_value},
        lease_path{std::move(lease_path_value)} {}

  inline static std::atomic_uint64_t next_lease_identity{1};
  int descriptor{-1};
  void *mapping{};
  std::size_t byte_size{};
  std::filesystem::path lease_path;
};

namespace {

// These offsets mirror the public dyld_cache_header layout. The parser uses
// mappingOffset as the header-availability boundary, matching dyld's own
// version gating instead of assuming every cache has the newest fields.
constexpr std::size_t magic_offset = 0;
constexpr std::size_t magic_size = 16;
constexpr std::size_t mapping_offset_field = 16;
constexpr std::size_t mapping_count_field = 20;
constexpr std::size_t images_offset_old_field = 24;
constexpr std::size_t images_count_old_field = 28;
constexpr std::size_t uuid_field = 88;
constexpr std::size_t images_text_offset_field = 136;
constexpr std::size_t images_text_count_field = 144;
constexpr std::size_t platform_field = 216;
constexpr std::size_t format_version_field = 220;
constexpr std::size_t shared_region_start_field = 224;
constexpr std::size_t shared_region_size_field = 232;
constexpr std::size_t max_slide_field = 240;
constexpr std::size_t mapping_with_slide_offset_field = 312;
constexpr std::size_t subcache_array_offset_field = 392;
constexpr std::size_t subcache_array_count_field = 396;
constexpr std::size_t images_offset_field = 448;
constexpr std::size_t images_count_field = 452;
constexpr std::size_t cache_subtype_field = 456;

constexpr std::size_t mapping_info_size = 32;
constexpr std::size_t mapping_with_slide_info_size = 56;
constexpr std::size_t image_info_size = 32;
constexpr std::size_t image_text_info_size = 32;
constexpr std::size_t subcache_v1_size = 24;
constexpr std::size_t subcache_size = 56;
constexpr std::size_t maximum_mapping_count = 64;
constexpr std::size_t maximum_image_count = 1'000'000;
constexpr std::size_t maximum_subcache_count = 64;
constexpr std::uint32_t mach_header_magic = 0xfeedfaceU;
constexpr std::uint32_t lc_segment = 0x1U;
constexpr std::uint32_t lc_uuid = 0x1bU;

struct TextInfo {
  DyldCacheUuid uuid{};
  std::uint64_t load_address{};
  std::uint32_t segment_size{};
  std::string path;
};

struct ParsedFile {
  DyldCacheFile file;
  std::string magic;
  std::uint32_t mapping_offset{};
  std::vector<TextInfo> text_infos;
};

[[nodiscard]] std::optional<GuestFileGeneration>
read_file_generation(const std::filesystem::path &path) {
  struct stat file_stat {};
  if (::stat(path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
      file_stat.st_size < 0) {
    return std::nullopt;
  }
  return GuestFileGeneration{
      static_cast<std::uint64_t>(file_stat.st_dev),
      static_cast<std::uint64_t>(file_stat.st_ino),
      static_cast<std::uint64_t>(file_stat.st_size),
      static_cast<std::int64_t>(file_stat.st_mtim.tv_sec),
      static_cast<std::int64_t>(file_stat.st_mtim.tv_nsec),
      static_cast<std::int64_t>(file_stat.st_ctim.tv_sec),
      static_cast<std::int64_t>(file_stat.st_ctim.tv_nsec)};
}

[[nodiscard]] bool add_overflows(std::uint64_t left, std::uint64_t right) {
  return right > std::numeric_limits<std::uint64_t>::max() - left;
}

[[nodiscard]] bool table_fits(std::uint64_t offset, std::uint64_t count,
                              std::uint64_t element_size,
                              std::uintmax_t file_size,
                              std::uint64_t maximum_count) {
  if (element_size == 0U || count > maximum_count ||
      count > std::numeric_limits<std::uint64_t>::max() / element_size) {
    return false;
  }
  const auto byte_count = count * element_size;
  return offset <= file_size && byte_count <= file_size - offset;
}

[[nodiscard]] bool read_exact(std::ifstream &input, std::uint64_t offset,
                              std::span<std::byte> destination) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max()) ||
      destination.size() > static_cast<std::size_t>(
                                std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  input.clear();
  input.seekg(static_cast<std::streamoff>(offset));
  if (!input) return false;
  input.read(reinterpret_cast<char *>(destination.data()),
             static_cast<std::streamsize>(destination.size()));
  return input && input.gcount() ==
                       static_cast<std::streamsize>(destination.size());
}

[[nodiscard]] std::optional<std::uint32_t>
read_u32(std::ifstream &input, std::uint64_t offset) {
  std::array<std::byte, sizeof(std::uint32_t)> bytes{};
  if (!read_exact(input, offset, bytes)) return std::nullopt;
  return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1]))
          << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2]))
          << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]))
          << 24U);
}

[[nodiscard]] std::optional<std::uint64_t>
read_u64(std::ifstream &input, std::uint64_t offset) {
  std::array<std::byte, sizeof(std::uint64_t)> bytes{};
  if (!read_exact(input, offset, bytes)) return std::nullopt;
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result |= static_cast<std::uint64_t>(
                  std::to_integer<std::uint8_t>(bytes[index]))
              << static_cast<unsigned>(index * 8U);
  }
  return result;
}

[[nodiscard]] std::optional<std::vector<std::byte>>
read_blob(std::ifstream &input, std::uint64_t offset, std::size_t size) {
  std::vector<std::byte> bytes(size);
  if (!read_exact(input, offset, bytes)) return std::nullopt;
  return bytes;
}

[[nodiscard]] std::optional<std::string>
read_string(std::ifstream &input, std::uint64_t offset,
            std::uintmax_t file_size) {
  if (offset >= file_size) return std::nullopt;
  constexpr std::size_t chunk_size = 256;
  std::string result;
  for (std::uint64_t cursor = offset; cursor < file_size;) {
    const auto remaining = file_size - cursor;
    const auto count = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, chunk_size));
    const auto chunk = read_blob(input, cursor, count);
    if (!chunk) return std::nullopt;
    for (const auto byte : *chunk) {
      if (byte == std::byte{}) return result;
      result.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    }
    if (result.size() > 4096U) return std::nullopt;
    cursor += count;
  }
  return std::nullopt;
}

[[nodiscard]] bool valid_magic(std::string_view magic) {
  return magic.size() == magic_size && magic.starts_with("dyld_v1");
}

[[nodiscard]] bool field_available(std::uint32_t mapping_offset,
                                   std::size_t field_offset,
                                   std::size_t field_size) {
  return mapping_offset > field_offset &&
         mapping_offset - field_offset >= field_size;
}

[[nodiscard]] std::optional<DyldCacheUuid>
read_uuid(std::ifstream &input, std::uint64_t offset) {
  DyldCacheUuid uuid{};
  if (!read_exact(input, offset, uuid)) return std::nullopt;
  return uuid;
}

[[nodiscard]] std::optional<ParsedFile>
parse_file(const std::filesystem::path &path, std::uint32_t file_index,
           std::uint64_t cache_vm_offset, std::string file_suffix,
           bool parse_images) {
  std::error_code error;
  const auto file_size = std::filesystem::file_size(path, error);
  if (error || file_size < magic_size) return std::nullopt;
  const auto file_generation = read_file_generation(path);
  const auto content_identity = shared_file_identity(path).content_identity;
  if (!file_generation || !content_identity) return std::nullopt;

  std::ifstream input{path, std::ios::binary};
  if (!input) return std::nullopt;
  const auto magic_bytes = read_blob(input, magic_offset, magic_size);
  if (!magic_bytes) return std::nullopt;
  const std::string magic{
      reinterpret_cast<const char *>(magic_bytes->data()), magic_bytes->size()};
  if (!valid_magic(magic)) return std::nullopt;

  const auto mapping_offset = read_u32(input, mapping_offset_field);
  const auto mapping_count = read_u32(input, mapping_count_field);
  if (!mapping_offset || !mapping_count || *mapping_offset > 1024U ||
      *mapping_count == 0U || *mapping_count > maximum_mapping_count) {
    return std::nullopt;
  }
  const bool has_mapping_with_slide =
      field_available(*mapping_offset, mapping_with_slide_offset_field,
                      sizeof(std::uint32_t));
  const auto mapping_size = has_mapping_with_slide
                                ? mapping_with_slide_info_size
                                : mapping_info_size;
  const auto mapping_table_offset = has_mapping_with_slide
                                        ? read_u32(input,
                                                   mapping_with_slide_offset_field)
                                        : std::optional<std::uint32_t>{
                                              *mapping_offset};
  if (!mapping_table_offset || *mapping_table_offset == 0U ||
      !table_fits(*mapping_table_offset, *mapping_count, mapping_size,
                  file_size, maximum_mapping_count)) {
    return std::nullopt;
  }

  ParsedFile parsed;
  parsed.file.path = path;
  parsed.file.file_size = file_size;
  parsed.file.file_generation = *file_generation;
  parsed.file.content_identity = *content_identity;
  parsed.file.cache_vm_offset = cache_vm_offset;
  parsed.file.file_suffix = std::move(file_suffix);
  parsed.magic = magic;
  parsed.mapping_offset = *mapping_offset;
  if (field_available(*mapping_offset, uuid_field, 16U)) {
    const auto uuid = read_uuid(input, uuid_field);
    if (!uuid) return std::nullopt;
    parsed.file.uuid = *uuid;
  }

  parsed.file.mappings.reserve(*mapping_count);
  for (std::uint32_t index = 0; index < *mapping_count; ++index) {
    const auto offset = static_cast<std::uint64_t>(*mapping_table_offset) +
                        static_cast<std::uint64_t>(index) * mapping_size;
    const auto address = read_u64(input, offset);
    const auto size = read_u64(input, offset + 8U);
    const auto file_offset = read_u64(input, offset + 16U);
    if (!address || !size || !file_offset || *size == 0U ||
        add_overflows(*address, *size) || add_overflows(*file_offset, *size) ||
        *file_offset + *size > file_size) {
      return std::nullopt;
    }
    const auto max_protection = read_u32(
        input, offset + (has_mapping_with_slide ? 48U : 24U));
    const auto initial_protection = read_u32(
        input, offset + (has_mapping_with_slide ? 52U : 28U));
    if (!max_protection || !initial_protection) return std::nullopt;

    DyldCacheMapping mapping;
    mapping.address = *address;
    mapping.size = *size;
    mapping.file_offset = *file_offset;
    mapping.file_index = file_index;
    mapping.maximum_protection = *max_protection;
    mapping.initial_protection = *initial_protection;
    if (has_mapping_with_slide) {
      const auto slide_offset = read_u64(input, offset + 24U);
      const auto slide_size = read_u64(input, offset + 32U);
      const auto flags = read_u64(input, offset + 40U);
      if (!slide_offset || !slide_size || !flags ||
          (*slide_size != 0U &&
           (add_overflows(*slide_offset, *slide_size) ||
            *slide_offset + *slide_size > file_size))) {
        return std::nullopt;
      }
      mapping.slide_info_offset = *slide_offset;
      mapping.slide_info_size = *slide_size;
      mapping.flags = *flags;
      if (*slide_size >= sizeof(std::uint32_t)) {
        const auto version = read_u32(input, *slide_offset);
        if (!version) return std::nullopt;
        mapping.slide_info_version = *version;
      }
    }

    for (const auto &previous : parsed.file.mappings) {
      const auto overlaps =
          mapping.address < previous.address + previous.size &&
          previous.address < mapping.address + mapping.size;
      if (overlaps) return std::nullopt;
    }
    parsed.file.mappings.push_back(std::move(mapping));
  }

  if (!parse_images) return parsed;
  if (field_available(*mapping_offset, images_text_count_field,
                      sizeof(std::uint64_t))) {
    const auto text_offset = read_u64(input, images_text_offset_field);
    const auto text_count = read_u64(input, images_text_count_field);
    if (!text_offset || !text_count ||
        !table_fits(*text_offset, *text_count, image_text_info_size, file_size,
                    maximum_image_count)) {
      return std::nullopt;
    }
    parsed.text_infos.reserve(static_cast<std::size_t>(*text_count));
    for (std::uint64_t index = 0; index < *text_count; ++index) {
      const auto offset = *text_offset + index * image_text_info_size;
      const auto uuid = read_uuid(input, offset);
      const auto load_address = read_u64(input, offset + 16U);
      const auto segment_size = read_u32(input, offset + 24U);
      const auto path_offset = read_u32(input, offset + 28U);
      if (!uuid || !load_address || !segment_size || !path_offset) {
        return std::nullopt;
      }
      const auto text_path = read_string(input, *path_offset, file_size);
      if (!text_path) return std::nullopt;
      parsed.text_infos.push_back(
          TextInfo{*uuid, *load_address, *segment_size, *text_path});
    }
  }
  return parsed;
}

struct CacheFileRange {
  std::uint32_t file_index{};
  std::uint64_t file_offset{};
};

[[nodiscard]] std::optional<CacheFileRange>
cache_file_for_vm(std::span<const DyldCacheFile> files,
                  std::uint64_t address, std::uint64_t size = 1U) {
  for (std::size_t index = 0; index < files.size(); ++index) {
    for (const auto &mapping : files[index].mappings) {
      if (address < mapping.address ||
          address - mapping.address > mapping.size ||
          size > mapping.size - (address - mapping.address)) {
        continue;
      }
      return CacheFileRange{static_cast<std::uint32_t>(index),
                            mapping.file_offset + (address - mapping.address)};
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t>
cache_file_index_for_file_range(std::span<const DyldCacheFile> files,
                                std::uint64_t file_offset,
                                std::uint64_t size) {
  for (std::size_t index = 0; index < files.size(); ++index) {
    for (const auto &mapping : files[index].mappings) {
      if (file_offset < mapping.file_offset ||
          file_offset - mapping.file_offset > mapping.size ||
          size > mapping.size - (file_offset - mapping.file_offset)) {
        continue;
      }
      return static_cast<std::uint32_t>(index);
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::uint32_t
read_span_u32(std::span<const std::byte> bytes, std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t))
    return 0U;
  return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U]))
          << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U]))
          << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U]))
          << 24U);
}

[[nodiscard]] std::optional<std::string>
read_span_fixed_string(std::span<const std::byte> bytes, std::size_t offset,
                       std::size_t size) {
  if (offset > bytes.size() || size > bytes.size() - offset) return std::nullopt;
  const auto end = std::find(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             bytes.begin() + static_cast<std::ptrdiff_t>(offset + size),
                             std::byte{});
  return std::string{reinterpret_cast<const char *>(bytes.data() + offset),
                     static_cast<std::size_t>(end -
                                               (bytes.begin() +
                                                static_cast<std::ptrdiff_t>(offset)))};
}

// Old shared caches predate the imagesText table. Their image records still
// provide the load address, so recover the image's Mach-O segments from the
// cache itself. This keeps HLE, catalog and shared-region mapping on one
// metadata path without identifying a particular firmware build.
bool populate_legacy_image_ranges(DyldCacheImage &image,
                                  std::span<const DyldCacheFile> files) {
  if (!image.executable_ranges.empty()) return true;
  const auto header_source = cache_file_for_vm(files, image.unslid_load_address);
  if (!header_source || header_source->file_index >= files.size()) return false;
  const auto &header_file = files[header_source->file_index];
  std::ifstream input{header_file.path, std::ios::binary};
  if (!input) return false;
  const auto magic = read_u32(input, header_source->file_offset);
  const auto command_count = read_u32(input, header_source->file_offset + 16U);
  const auto command_bytes = read_u32(input, header_source->file_offset + 20U);
  if (!magic || !command_count || !command_bytes || *magic != mach_header_magic ||
      *command_bytes > header_file.file_size ||
      header_source->file_offset > header_file.file_size - 28U ||
      *command_bytes >
          header_file.file_size - header_source->file_offset - 28U) {
    return false;
  }
  const auto command_blob = read_blob(
      input, header_source->file_offset + 28U, *command_bytes);
  if (!command_blob) return false;
  const std::span<const std::byte> commands{*command_blob};
  std::size_t cursor = 0;
  std::optional<DyldCacheRange> text_range;
  for (std::uint32_t index = 0; index < *command_count; ++index) {
    if (cursor > commands.size() || commands.size() - cursor < 8U) return false;
    const auto command = read_span_u32(commands, cursor);
    const auto size = read_span_u32(commands, cursor + 4U);
    if (size < 8U || size > commands.size() - cursor) return false;
    if (command == lc_segment && size >= 56U) {
      const auto name = read_span_fixed_string(commands, cursor + 8U, 16U);
      if (!name) return false;
      const auto address = read_span_u32(commands, cursor + 24U);
      const auto vm_size = read_span_u32(commands, cursor + 28U);
      const auto file_offset = read_span_u32(commands, cursor + 32U);
      const auto file_size = read_span_u32(commands, cursor + 36U);
      const auto maximum_protection = read_span_u32(commands, cursor + 40U);
      const auto initial_protection = read_span_u32(commands, cursor + 44U);
      if (file_size != 0U && (initial_protection & 4U) != 0U) {
        // In the legacy cache format __TEXT's fileoff is relative to the
        // image header, while the later segments use absolute cache offsets.
        // Keep this rule tied to the Mach-O segment contract rather than to a
        // firmware/build identifier.
        const bool image_relative = *name == "__TEXT";
        if (image_relative &&
            add_overflows(header_source->file_offset, file_offset)) {
          return false;
        }
        const auto actual_file_offset =
            image_relative ? header_source->file_offset + file_offset
                            : file_offset;
        const auto source = cache_file_index_for_file_range(
            files, actual_file_offset, file_size);
        if (source) {
          const auto range = DyldCacheRange{
              address, file_size, actual_file_offset, *source,
              initial_protection, maximum_protection};
          image.executable_ranges.push_back(range);
          if (*name == "__TEXT") text_range = range;
        }
      }
      if (*name == "__TEXT") image.text_segment_size = vm_size;
    } else if (command == lc_uuid && size >= 24U) {
      DyldCacheUuid uuid{};
      std::copy_n(commands.begin() + static_cast<std::ptrdiff_t>(cursor + 8U),
                  uuid.size(), uuid.begin());
      image.text_uuid = uuid;
    }
    cursor += size;
  }
  if (cursor != commands.size()) return false;
  if (text_range) {
    if (text_range->file_index >= files.size()) return false;
    if (const auto identity = sha256_file(
            files[text_range->file_index].path, text_range->file_offset,
            text_range->size)) {
      image.text_identity = *identity;
    }
  }
  return !image.executable_ranges.empty();
}

[[nodiscard]] std::filesystem::path
discover_subcache(const std::filesystem::path &main_path,
                  std::string_view suffix, std::size_t index) {
  if (!suffix.empty()) {
    return main_path.parent_path() /
           (main_path.filename().string() + std::string{suffix});
  }
  const auto number = index + 1U;
  std::string fallback = main_path.filename().string() + ".";
  fallback.push_back(static_cast<char>('0' + (number / 10U) % 10U));
  fallback.push_back(static_cast<char>('0' + number % 10U));
  return main_path.parent_path() / fallback;
}

void append_u32(std::vector<std::byte> &bytes, std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
  }
}

void append_u64(std::vector<std::byte> &bytes, std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
  }
}

void append_identity(std::vector<std::byte> &bytes,
                     const ContentIdentity &identity) {
  bytes.insert(bytes.end(), identity.digest.begin(), identity.digest.end());
}

void append_uuid(std::vector<std::byte> &bytes, const DyldCacheUuid &uuid) {
  bytes.insert(bytes.end(), uuid.begin(), uuid.end());
}

void append_generation(std::vector<std::byte> &bytes,
                       const GuestFileGeneration &generation) {
  append_u64(bytes, generation.device);
  append_u64(bytes, generation.inode);
  append_u64(bytes, generation.file_size);
  append_u64(bytes, static_cast<std::uint64_t>(generation.modified_seconds));
  append_u64(bytes,
             static_cast<std::uint64_t>(generation.modified_nanoseconds));
  append_u64(bytes, static_cast<std::uint64_t>(generation.changed_seconds));
  append_u64(bytes,
             static_cast<std::uint64_t>(generation.changed_nanoseconds));
}

void append_u8(std::vector<std::byte> &bytes, std::uint8_t value) {
  bytes.push_back(static_cast<std::byte>(value));
}

void append_presence(std::vector<std::byte> &bytes, bool present) {
  append_u8(bytes, present ? 1U : 0U);
}

void append_string(std::vector<std::byte> &bytes, std::string_view value) {
  append_u32(bytes, static_cast<std::uint32_t>(value.size()));
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(value.data()),
               reinterpret_cast<const std::byte *>(value.data() + value.size()));
}

void append_optional_generation(
    std::vector<std::byte> &bytes,
    const std::optional<GuestFileGeneration> &generation) {
  append_presence(bytes, generation.has_value());
  if (generation) append_generation(bytes, *generation);
}

void append_optional_identity(
    std::vector<std::byte> &bytes,
    const std::optional<ContentIdentity> &identity) {
  append_presence(bytes, identity.has_value());
  if (identity) append_identity(bytes, *identity);
}

void append_optional_uuid(std::vector<std::byte> &bytes,
                          const std::optional<DyldCacheUuid> &uuid) {
  append_presence(bytes, uuid.has_value());
  if (uuid) append_uuid(bytes, *uuid);
}

class ArtifactReader {
public:
  explicit ArtifactReader(std::span<const std::byte> bytes) : bytes_{bytes} {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }

  [[nodiscard]] std::optional<std::uint8_t> u8() {
    if (!take(sizeof(std::uint8_t))) return std::nullopt;
    return std::to_integer<std::uint8_t>(bytes_[offset_++]);
  }

  [[nodiscard]] std::optional<std::uint32_t> u32() {
    if (!take(sizeof(std::uint32_t))) return std::nullopt;
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      value |= static_cast<std::uint32_t>(
                   std::to_integer<std::uint8_t>(bytes_[offset_ + index]))
               << static_cast<unsigned>(index * 8U);
    }
    offset_ += sizeof(value);
    return value;
  }

  [[nodiscard]] std::optional<std::uint64_t> u64() {
    if (!take(sizeof(std::uint64_t))) return std::nullopt;
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
      value |= static_cast<std::uint64_t>(
                   std::to_integer<std::uint8_t>(bytes_[offset_ + index]))
               << static_cast<unsigned>(index * 8U);
    }
    offset_ += sizeof(value);
    return value;
  }

  [[nodiscard]] std::optional<std::string> string(
      std::size_t maximum_size = 1U << 20U) {
    const auto size = u32();
    if (!size || *size > maximum_size || !take(*size)) return std::nullopt;
    std::string value{reinterpret_cast<const char *>(bytes_.data() + offset_),
                      *size};
    offset_ += *size;
    return value;
  }

  [[nodiscard]] std::optional<ContentIdentity> identity() {
    if (!take(32U)) return std::nullopt;
    ContentIdentity identity;
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                identity.digest.size(), identity.digest.begin());
    offset_ += identity.digest.size();
    return identity;
  }

  [[nodiscard]] std::optional<DyldCacheUuid> uuid() {
    if (!take(16U)) return std::nullopt;
    DyldCacheUuid uuid;
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                uuid.size(), uuid.begin());
    offset_ += uuid.size();
    return uuid;
  }

  [[nodiscard]] std::optional<std::span<const std::byte>> fixed(
      std::size_t size) {
    if (!take(size)) return std::nullopt;
    const auto result = bytes_.subspan(offset_, size);
    offset_ += size;
    return result;
  }

private:
  [[nodiscard]] bool take(std::size_t size) {
    if (size > bytes_.size() - std::min(offset_, bytes_.size())) {
      ok_ = false;
      return false;
    }
    return true;
  }

  std::span<const std::byte> bytes_;
  std::size_t offset_{};
  bool ok_{true};
};

[[nodiscard]] std::string normalize_cache_path(
    const std::filesystem::path &path);

[[nodiscard]] std::string generation_artifact_name(
    const std::filesystem::path &path,
    const DyldSharedCacheOptions &options,
    const GuestFileGeneration &main_generation,
    const ContentIdentity &main_identity) {
  std::vector<std::byte> key;
  const auto append_key_string = [&key](std::string_view value) {
    append_u32(key, static_cast<std::uint32_t>(value.size()));
    key.insert(key.end(), reinterpret_cast<const std::byte *>(value.data()),
               reinterpret_cast<const std::byte *>(value.data() + value.size()));
  };
  append_key_string("ilemu-dyld-generation-v1");
  append_u32(key, DyldSharedCache::parser_schema_version);
  append_u32(key, DyldSharedCache::hle_profile_schema_version);
  append_key_string(normalize_cache_path(path));
  append_key_string(options.architecture);
  for (const auto &subcache : options.subcache_paths)
    append_key_string(normalize_cache_path(subcache));
  append_identity(key, main_identity);
  append_generation(key, main_generation);
  return "dyld-generation-" + sha256(key).hex() + ".artifact";
}

[[nodiscard]] bool artifact_magic(ArtifactReader &reader,
                                  std::string_view expected) {
  const auto bytes = reader.fixed(expected.size());
  return bytes && std::string_view{
                         reinterpret_cast<const char *>(bytes->data()),
                         bytes->size()} == expected;
}

[[nodiscard]] std::string normalize_cache_path(
    const std::filesystem::path &path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  return (error ? path : absolute).lexically_normal().generic_string();
}

struct GenerationCacheEntry {
  std::weak_ptr<const DyldSharedCache> generation;
  std::vector<DyldCacheFile> files;
};

std::mutex generation_cache_mutex;
std::map<std::string, GenerationCacheEntry, std::less<>> generation_cache;
std::uint64_t generation_builds{};
std::uint64_t generation_hits{};
std::uint64_t generation_artifact_builds{};
std::uint64_t generation_artifact_hits{};
std::uint64_t image_builds{};
std::uint64_t image_hits{};

[[nodiscard]] std::string generation_cache_key(
    const std::filesystem::path &path, const DyldSharedCacheOptions &options) {
  std::string key = std::to_string(DyldSharedCache::parser_schema_version);
  key.push_back('\n');
  key += std::to_string(DyldSharedCache::hle_profile_schema_version);
  key.push_back('\n');
  key += normalize_cache_path(path);
  key.push_back('\n');
  key += options.architecture;
  for (const auto &subcache : options.subcache_paths) {
    key.push_back('\n');
    key += normalize_cache_path(subcache);
  }
  return key;
}

[[nodiscard]] bool generation_files_are_current(
    const GenerationCacheEntry &entry) {
  for (const auto &file : entry.files) {
    const auto generation = read_file_generation(file.path);
    if (!generation || !file.file_generation ||
        *generation != *file.file_generation) {
      return false;
    }
    const auto identity = shared_file_identity(file.path).content_identity;
    if (!identity || *identity != file.content_identity) return false;
  }
  return true;
}

[[nodiscard]] std::shared_ptr<const ImmutableFileView>
read_immutable_file_view(const DyldCacheFile &file) {
  if (!file.file_generation ||
      file.file_size > std::numeric_limits<std::size_t>::max()) {
    return {};
  }
  return ImmutableFileView::open(file.path, *file.file_generation,
                                 file.content_identity,
                                 static_cast<std::uint64_t>(file.file_size));
}

} // namespace

DyldSharedCache::ParseResult::ParseResult(
    std::shared_ptr<const DyldSharedCache> generation) noexcept
    : generation_{std::move(generation)} {}

bool DyldSharedCache::ParseResult::has_value() const noexcept {
  return generation_ != nullptr;
}

DyldSharedCache::ParseResult::operator bool() const noexcept {
  return has_value();
}

const DyldSharedCache *DyldSharedCache::ParseResult::operator->()
    const noexcept {
  return generation_.get();
}

const DyldSharedCache &DyldSharedCache::ParseResult::operator*() const noexcept {
  return *generation_;
}

std::shared_ptr<const DyldSharedCache>
DyldSharedCache::ParseResult::shared() const noexcept {
  return generation_;
}

DyldSharedCache::DyldSharedCache()
    : image_store_{std::make_shared<ImageStore>()} {}

DyldSharedCache::~DyldSharedCache() = default;

DyldSharedCache::DyldSharedCache(DyldSharedCache &&other) noexcept = default;

DyldSharedCache &DyldSharedCache::operator=(DyldSharedCache &&other) noexcept =
    default;

std::shared_ptr<const DyldSharedCache>
DyldSharedCache::load_shared_generation_artifact(
    const std::filesystem::path &path, const DyldSharedCacheOptions &options,
    const GuestFileGeneration &main_generation,
    const ContentIdentity &main_identity) {
  const auto artifact = GenerationArtifactView::open(
      shared_immutable_artifact_named_path(generation_artifact_name(
          path, options, main_generation, main_identity)));
  if (!artifact) return {};

  ArtifactReader reader{artifact->bytes()};
  if (!artifact_magic(reader, "ILEMU-DYLD-GEN")) return {};
  const auto version = reader.u32();
  if (!version || *version != 1U) return {};
  const auto file_count = reader.u32();
  if (!file_count || *file_count == 0U || *file_count > maximum_subcache_count +
                                                1U) {
    return {};
  }

  DyldSharedCache result;
  result.generation_artifact_view_ = artifact;
  result.files_.reserve(*file_count);
  for (std::uint32_t file_index = 0; file_index < *file_count;
       ++file_index) {
    const auto file_path = reader.string();
    const auto file_size = reader.u64();
    const auto generation_present = reader.u8();
    if (!file_path || !file_size || !generation_present ||
        *generation_present > 1U) {
      return {};
    }
    std::optional<GuestFileGeneration> generation;
    if (*generation_present != 0U) {
      const auto device = reader.u64();
      const auto inode = reader.u64();
      const auto size = reader.u64();
      const auto modified_seconds = reader.u64();
      const auto modified_nanoseconds = reader.u64();
      const auto changed_seconds = reader.u64();
      const auto changed_nanoseconds = reader.u64();
      if (!device || !inode || !size || !modified_seconds ||
          !modified_nanoseconds || !changed_seconds || !changed_nanoseconds) {
        return {};
      }
      generation = GuestFileGeneration{
          *device,
          *inode,
          *size,
          static_cast<std::int64_t>(*modified_seconds),
          static_cast<std::int64_t>(*modified_nanoseconds),
          static_cast<std::int64_t>(*changed_seconds),
          static_cast<std::int64_t>(*changed_nanoseconds)};
    }
    const auto identity = reader.identity();
    const auto uuid = reader.uuid();
    const auto cache_vm_offset = reader.u64();
    const auto suffix = reader.string(4096U);
    const auto mapping_count = reader.u32();
    if (!identity || !uuid || !cache_vm_offset || !suffix || !mapping_count ||
        *mapping_count > maximum_mapping_count || !generation) {
      return {};
    }

    DyldCacheFile file;
    file.path = std::filesystem::path{*file_path};
    file.file_size = static_cast<std::uintmax_t>(*file_size);
    file.file_generation = *generation;
    file.content_identity = *identity;
    file.uuid = *uuid;
    file.cache_vm_offset = *cache_vm_offset;
    file.file_suffix = *suffix;
    file.mappings.reserve(*mapping_count);
    for (std::uint32_t mapping_index = 0; mapping_index < *mapping_count;
         ++mapping_index) {
      const auto address = reader.u64();
      const auto size = reader.u64();
      const auto file_offset = reader.u64();
      const auto serialized_file_index = reader.u32();
      const auto initial_protection = reader.u32();
      const auto maximum_protection = reader.u32();
      const auto slide_info_offset = reader.u64();
      const auto slide_info_size = reader.u64();
      const auto flags = reader.u64();
      const auto slide_version_present = reader.u8();
      if (!address || !size || !file_offset || !serialized_file_index ||
          !initial_protection || !maximum_protection || !slide_info_offset ||
          !slide_info_size || !flags || !slide_version_present ||
          *slide_version_present > 1U ||
          *serialized_file_index != file_index || *size == 0U ||
          add_overflows(*address, *size) ||
          add_overflows(*file_offset, *size) ||
          *file_offset + *size > *file_size) {
        return {};
      }
      DyldCacheMapping mapping;
      mapping.address = *address;
      mapping.size = *size;
      mapping.file_offset = *file_offset;
      mapping.file_index = *serialized_file_index;
      mapping.initial_protection = *initial_protection;
      mapping.maximum_protection = *maximum_protection;
      mapping.slide_info_offset = *slide_info_offset;
      mapping.slide_info_size = *slide_info_size;
      mapping.flags = *flags;
      if (*slide_version_present != 0U) {
        const auto slide_version = reader.u32();
        if (!slide_version) return {};
        mapping.slide_info_version = *slide_version;
      }
      file.mappings.push_back(std::move(mapping));
    }
    result.files_.push_back(std::move(file));
  }

  if (normalize_cache_path(result.files_.front().path) !=
      normalize_cache_path(path)) {
    return {};
  }
  if (!options.subcache_paths.empty() &&
      options.subcache_paths.size() + 1U != result.files_.size()) {
    return {};
  }
  for (std::size_t index = 1; index < result.files_.size(); ++index) {
    if (!options.subcache_paths.empty() &&
        normalize_cache_path(options.subcache_paths[index - 1U]) !=
            normalize_cache_path(result.files_[index].path)) {
      return {};
    }
  }

  const auto platform = reader.u32();
  const auto format = reader.u8();
  const auto shared_region_start = reader.u64();
  const auto shared_region_size = reader.u64();
  const auto max_slide = reader.u64();
  const auto image_count = reader.u32();
  if (!platform || !format || !shared_region_start || !shared_region_size ||
      !max_slide || !image_count || *image_count > maximum_image_count) {
    return {};
  }
  result.platform_ = *platform;
  result.format_version_ = *format;
  result.shared_region_start_ = *shared_region_start;
  result.shared_region_size_ = *shared_region_size;
  result.max_slide_ = *max_slide;
  result.images_.reserve(*image_count);
  for (std::uint32_t image_index = 0; image_index < *image_count;
       ++image_index) {
    const auto serialized_index = reader.u32();
    const auto image_path = reader.string(4096U);
    const auto load_address = reader.u64();
    const auto text_uuid_present = reader.u8();
    if (!serialized_index || !image_path || !load_address ||
        !text_uuid_present || *text_uuid_present > 1U ||
        *serialized_index != image_index) {
      return {};
    }
    std::optional<DyldCacheUuid> text_uuid;
    if (*text_uuid_present != 0U) {
      const auto uuid = reader.uuid();
      if (!uuid) {
        return {};
      }
      text_uuid = *uuid;
    }
    const auto text_segment_size = reader.u64();
    const auto range_count = reader.u32();
    if (!text_segment_size ||
        !range_count || *range_count > maximum_mapping_count * 16U ||
        !reader.ok()) {
      return {};
    }
    DyldCacheImage image;
    image.index = *serialized_index;
    image.path = *image_path;
    image.unslid_load_address = *load_address;
    image.text_segment_size = *text_segment_size;
    image.text_uuid = text_uuid;
    image.executable_ranges.reserve(*range_count);
    for (std::uint32_t range_index = 0; range_index < *range_count;
         ++range_index) {
      const auto address = reader.u64();
      const auto size = reader.u64();
      const auto file_offset = reader.u64();
      const auto file_index = reader.u32();
      const auto initial_protection = reader.u32();
      const auto maximum_protection = reader.u32();
      if (!address || !size || !file_offset || !file_index ||
          !initial_protection || !maximum_protection ||
          *file_index >= result.files_.size() ||
          *size == 0U || add_overflows(*file_offset, *size) ||
          *file_offset + *size > result.files_[*file_index].file_size) {
        return {};
      }
      image.executable_ranges.push_back(DyldCacheRange{
          *address, *size, *file_offset, *file_index, *initial_protection,
          *maximum_protection});
    }
    const auto image_identity_present = reader.u8();
    if (!image_identity_present || *image_identity_present > 1U) {
      return {};
    }
    if (*image_identity_present != 0U) {
      const auto identity = reader.identity();
      if (!identity) {
        return {};
      }
      image.text_identity = *identity;
    }
    result.images_.push_back(std::move(image));
  }
  const auto generation_identity = reader.identity();
  if (!generation_identity || !reader.ok()) {
    return {};
  }
  result.generation_identity_ = *generation_identity;

  for (auto &file : result.files_) {
    const auto current_generation = read_file_generation(file.path);
    const auto current_identity = shared_file_identity(file.path).content_identity;
    if (!current_generation || !current_identity ||
        *current_generation != *file.file_generation ||
        *current_identity != file.content_identity ||
        current_generation->file_size != file.file_size) {
      return {};
    }
    file.immutable_file_view = ImmutableFileView::open(
        file.path, *file.file_generation, file.content_identity,
        static_cast<std::uint64_t>(file.file_size));
    if (!file.immutable_file_view) return {};
  }
  result.main_cache_ = result.files_.front();

  result.image_range_index_.resize(result.files_.size());
  for (const auto &image : result.images_) {
    for (const auto &range : image.executable_ranges) {
      if (range.file_index >= result.image_range_index_.size() ||
          add_overflows(range.file_offset, range.size)) {
        continue;
      }
      result.image_range_index_[range.file_index].push_back(
          ImageRangeIndexEntry{range.file_offset,
                               range.file_offset + range.size, 0,
                               image.index});
    }
  }
  for (auto &entries : result.image_range_index_) {
    std::sort(entries.begin(), entries.end(),
              [](const ImageRangeIndexEntry &left,
                 const ImageRangeIndexEntry &right) {
                if (left.file_offset != right.file_offset)
                  return left.file_offset < right.file_offset;
                return left.image_index < right.image_index;
              });
    std::uint64_t prefix_file_end = 0;
    for (auto &entry : entries) {
      prefix_file_end = std::max(prefix_file_end, entry.file_end);
      entry.prefix_file_end = prefix_file_end;
    }
  }
  return std::make_shared<const DyldSharedCache>(std::move(result));
}

void DyldSharedCache::publish_shared_generation_artifact(
    const std::filesystem::path &path, const DyldSharedCacheOptions &options,
    const GuestFileGeneration &main_generation,
    const ContentIdentity &main_identity) const {
  std::vector<std::byte> bytes;
  bytes.reserve(1024U + images_.size() * 128U);
  const auto append_mapping = [&bytes](const DyldCacheMapping &mapping) {
    append_u64(bytes, mapping.address);
    append_u64(bytes, mapping.size);
    append_u64(bytes, mapping.file_offset);
    append_u32(bytes, mapping.file_index);
    append_u32(bytes, mapping.initial_protection);
    append_u32(bytes, mapping.maximum_protection);
    append_u64(bytes, mapping.slide_info_offset);
    append_u64(bytes, mapping.slide_info_size);
    append_u64(bytes, mapping.flags);
    append_presence(bytes, mapping.slide_info_version.has_value());
    if (mapping.slide_info_version) append_u32(bytes, *mapping.slide_info_version);
  };
  const auto append_range = [&bytes](const DyldCacheRange &range) {
    append_u64(bytes, range.address);
    append_u64(bytes, range.size);
    append_u64(bytes, range.file_offset);
    append_u32(bytes, range.file_index);
    append_u32(bytes, range.initial_protection);
    append_u32(bytes, range.maximum_protection);
  };

  const std::string_view magic{"ILEMU-DYLD-GEN"};
  bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(magic.data()),
               reinterpret_cast<const std::byte *>(magic.data() + magic.size()));
  append_u32(bytes, 1U);
  append_u32(bytes, static_cast<std::uint32_t>(files_.size()));
  for (const auto &file : files_) {
    append_string(bytes, file.path.generic_string());
    append_u64(bytes, static_cast<std::uint64_t>(file.file_size));
    append_optional_generation(bytes, file.file_generation);
    append_identity(bytes, file.content_identity);
    append_uuid(bytes, file.uuid);
    append_u64(bytes, file.cache_vm_offset);
    append_string(bytes, file.file_suffix);
    append_u32(bytes, static_cast<std::uint32_t>(file.mappings.size()));
    for (const auto &mapping : file.mappings) append_mapping(mapping);
  }
  append_u32(bytes, platform_);
  append_u8(bytes, format_version_);
  append_u64(bytes, shared_region_start_);
  append_u64(bytes, shared_region_size_);
  append_u64(bytes, max_slide_);
  append_u32(bytes, static_cast<std::uint32_t>(images_.size()));
  for (const auto &image : images_) {
    append_u32(bytes, image.index);
    append_string(bytes, image.path);
    append_u64(bytes, image.unslid_load_address);
    append_optional_uuid(bytes, image.text_uuid);
    append_u64(bytes, image.text_segment_size);
    append_u32(bytes, static_cast<std::uint32_t>(image.executable_ranges.size()));
    for (const auto &range : image.executable_ranges) append_range(range);
    append_optional_identity(bytes, image.text_identity);
  }
  append_identity(bytes, generation_identity_);
  static_cast<void>(options);
  static_cast<void>(main_generation);
  static_cast<void>(main_identity);
  static_cast<void>(publish_shared_immutable_artifact(
      shared_immutable_artifact_named_path(generation_artifact_name(
          path, options, main_generation, main_identity)),
      bytes));
}

DyldSharedCache::ParseResult
DyldSharedCache::parse(const std::filesystem::path &path,
                       DyldSharedCacheOptions options) {
  const auto cache_key = generation_cache_key(path, options);
  // Keep construction single-flight. The parser only holds this lock while a
  // generation is absent; once published, all callers retain the same
  // immutable object and do not copy its metadata.
  std::unique_lock generation_lock{generation_cache_mutex};
  if (const auto cached = generation_cache.find(cache_key);
      cached != generation_cache.end() &&
          generation_files_are_current(cached->second)) {
    if (const auto generation = cached->second.generation.lock()) {
      ++generation_hits;
      return generation;
    }
  }

  // A process-local weak pointer is only the fast path.  Independent
  // emulator processes acquire the same read-only serialized generation
  // metadata after validating the current main member's generation and
  // content identity; the loader then validates every subcache before
  // publishing the local handle.
  const auto main_generation = read_file_generation(path);
  const auto main_identity = shared_file_identity(path).content_identity;
  if (main_generation && main_identity) {
    if (const auto generation = load_shared_generation_artifact(
            path, options, *main_generation, *main_identity)) {
      generation_cache[cache_key] =
          GenerationCacheEntry{generation,
                               std::vector<DyldCacheFile>(
                                   generation->files().begin(),
                                   generation->files().end())};
      ++generation_hits;
      ++generation_artifact_hits;
      return generation;
    }
  }

  const auto main = parse_file(path, 0, 0, {}, true);
  if (!main) return {};

  std::ifstream header_input{path, std::ios::binary};
  if (!header_input) return {};
  DyldSharedCache result;
  result.main_cache_ = main->file;
  result.files_.push_back(main->file);

  const auto read_optional_u32 = [&](std::size_t offset) {
    return field_available(main->mapping_offset, offset,
                           sizeof(std::uint32_t))
               ? read_u32(header_input, offset)
               : std::optional<std::uint32_t>{0U};
  };
  const auto read_optional_u64 = [&](std::size_t offset) {
    return field_available(main->mapping_offset, offset,
                           sizeof(std::uint64_t))
               ? read_u64(header_input, offset)
               : std::optional<std::uint64_t>{0U};
  };
  const auto platform = read_optional_u32(platform_field);
  const auto format = read_optional_u32(format_version_field);
  const auto shared_start = read_optional_u64(shared_region_start_field);
  const auto shared_size = read_optional_u64(shared_region_size_field);
  const auto max_slide = read_optional_u64(max_slide_field);
  if (!platform || !format || !shared_start || !shared_size || !max_slide) {
    return {};
  }
  result.platform_ = *platform;
  result.format_version_ = static_cast<std::uint8_t>(*format & 0xffU);
  result.shared_region_start_ = *shared_start;
  result.shared_region_size_ = *shared_size;
  result.max_slide_ = *max_slide;

  std::uint32_t subcache_count = 0;
  std::uint32_t subcache_offset = 0;
  if (field_available(main->mapping_offset, subcache_array_count_field,
                      sizeof(std::uint32_t))) {
    const auto offset = read_u32(header_input, subcache_array_offset_field);
    const auto count = read_u32(header_input, subcache_array_count_field);
    if (!offset || !count) return {};
    subcache_offset = *offset;
    subcache_count = *count;
  }
  if (subcache_count > maximum_subcache_count) return {};
  const bool has_extended_subcache_entry =
      field_available(main->mapping_offset, cache_subtype_field,
                      sizeof(std::uint32_t));
  const auto subcache_entry_size =
      has_extended_subcache_entry ? subcache_size : subcache_v1_size;
  if (subcache_count != 0U &&
      !table_fits(subcache_offset, subcache_count, subcache_entry_size,
                  main->file.file_size, maximum_subcache_count)) {
    return {};
  }
  if (!options.subcache_paths.empty() &&
      options.subcache_paths.size() != subcache_count) {
    return {};
  }

  for (std::uint32_t index = 0; index < subcache_count; ++index) {
    const auto entry_offset = static_cast<std::uint64_t>(subcache_offset) +
                              static_cast<std::uint64_t>(index) *
                                  subcache_entry_size;
    const auto uuid = read_uuid(header_input, entry_offset);
    const auto vm_offset = read_u64(header_input, entry_offset + 16U);
    if (!uuid || !vm_offset) return {};
    std::string suffix;
    if (has_extended_subcache_entry) {
      const auto suffix_bytes = read_blob(header_input, entry_offset + 24U, 32U);
      if (!suffix_bytes) return {};
      const auto end = std::find(suffix_bytes->begin(), suffix_bytes->end(),
                                 std::byte{});
      suffix.assign(reinterpret_cast<const char *>(suffix_bytes->data()),
                    static_cast<std::size_t>(end - suffix_bytes->begin()));
      if (suffix.find('/') != std::string::npos ||
          suffix.find('\\') != std::string::npos) {
        return {};
      }
    }
    const auto subcache_path = options.subcache_paths.empty()
                                   ? discover_subcache(path, suffix, index)
                                   : options.subcache_paths[index];
    const auto subcache =
        parse_file(subcache_path, index + 1U, *vm_offset, suffix, false);
    if (!subcache || subcache->file.uuid != *uuid) return {};
    result.files_.push_back(subcache->file);
  }

  // Capture every member before publishing the generation. The shared
  // pointer is retained by the generation, so delayed image/HLE consumers
  // cannot accidentally reopen a replacement at the same pathname.
  for (auto &file : result.files_) {
    file.immutable_file_view = read_immutable_file_view(file);
    if (!file.immutable_file_view) return {};
  }
  result.main_cache_ = result.files_.front();

  std::uint32_t image_offset = 0;
  std::uint32_t image_count = 0;
  if (field_available(main->mapping_offset, images_count_field,
                      sizeof(std::uint32_t))) {
    const auto offset = read_u32(header_input, images_offset_field);
    const auto count = read_u32(header_input, images_count_field);
    if (!offset || !count) return {};
    image_offset = *offset;
    image_count = *count;
  } else {
    const auto offset = read_u32(header_input, images_offset_old_field);
    const auto count = read_u32(header_input, images_count_old_field);
    if (!offset || !count) return {};
    image_offset = *offset;
    image_count = *count;
  }
  if (!table_fits(image_offset, image_count, image_info_size,
                  main->file.file_size, maximum_image_count)) {
    return {};
  }
  result.images_.reserve(image_count);
  for (std::uint32_t index = 0; index < image_count; ++index) {
    const auto offset = static_cast<std::uint64_t>(image_offset) +
                        static_cast<std::uint64_t>(index) * image_info_size;
    const auto load_address = read_u64(header_input, offset);
    const auto path_offset = read_u32(header_input, offset + 24U);
    if (!load_address || !path_offset) return {};
    const auto image_path =
        read_string(header_input, *path_offset, main->file.file_size);
    if (!image_path) return {};
    DyldCacheImage image;
    image.index = index;
    image.path = *image_path;
    image.unslid_load_address = *load_address;
    result.images_.push_back(std::move(image));
  }

  for (const auto &info : main->text_infos) {
    const auto image = std::find_if(
        result.images_.begin(), result.images_.end(), [&](const auto &candidate) {
          return candidate.path == info.path ||
                 candidate.unslid_load_address == info.load_address;
        });
    if (image == result.images_.end()) continue;
    image->text_uuid = info.uuid;
    image->text_segment_size = info.segment_size;

    const DyldCacheMapping *mapping = nullptr;
    for (const auto &file : result.files_) {
      const auto candidate = std::find_if(
          file.mappings.begin(), file.mappings.end(), [&](const auto &range) {
            return (range.maximum_protection & 4U) != 0U &&
                   info.load_address >= range.address &&
                   info.load_address - range.address < range.size;
          });
      if (candidate != file.mappings.end()) {
        mapping = &*candidate;
        break;
      }
    }
    if (!mapping) continue;
    const auto size = std::min<std::uint64_t>(
        info.segment_size, mapping->size - (info.load_address - mapping->address));
    if (size == 0U || mapping->file_index >= result.files_.size() ||
        add_overflows(mapping->file_offset,
                      info.load_address - mapping->address)) {
      return {};
    }
    const auto file_offset =
        mapping->file_offset + (info.load_address - mapping->address);
    image->executable_ranges.push_back(
        DyldCacheRange{info.load_address, size, file_offset,
                       mapping->file_index, mapping->initial_protection,
                       mapping->maximum_protection});
    const auto identity = sha256_file(result.files_[mapping->file_index].path,
                                      file_offset, size);
    if (!identity) return {};
    image->text_identity = *identity;
  }

  for (auto &image : result.images_) {
    static_cast<void>(populate_legacy_image_ranges(image, result.files_));
  }
  result.image_range_index_.resize(result.files_.size());
  for (const auto &image : result.images_) {
    for (const auto &range : image.executable_ranges) {
      if (range.file_index >= result.image_range_index_.size() ||
          add_overflows(range.file_offset, range.size)) {
        continue;
      }
      result.image_range_index_[range.file_index].push_back(
          ImageRangeIndexEntry{range.file_offset,
                               range.file_offset + range.size, 0,
                               image.index});
    }
  }
  for (auto &entries : result.image_range_index_) {
    std::sort(entries.begin(), entries.end(),
              [](const ImageRangeIndexEntry &left,
                 const ImageRangeIndexEntry &right) {
                if (left.file_offset != right.file_offset)
                  return left.file_offset < right.file_offset;
                return left.image_index < right.image_index;
              });
    std::uint64_t prefix_file_end = 0;
    for (auto &entry : entries) {
      prefix_file_end = std::max(prefix_file_end, entry.file_end);
      entry.prefix_file_end = prefix_file_end;
    }
  }

  std::vector<std::byte> generation_key;
  generation_key.reserve(96U + result.files_.size() * 64U +
                         options.architecture.size());
  append_u32(generation_key, DyldSharedCache::parser_schema_version);
  append_u32(generation_key, DyldSharedCache::hle_profile_schema_version);
  const auto architecture = options.architecture.empty()
                                ? main->magic
                                : options.architecture;
  generation_key.insert(
      generation_key.end(),
      reinterpret_cast<const std::byte *>(architecture.data()),
      reinterpret_cast<const std::byte *>(architecture.data() +
                                          architecture.size()));
  append_u32(generation_key, result.platform_);
  append_u32(generation_key, result.format_version_);
  append_u64(generation_key, result.shared_region_start_);
  append_u64(generation_key, result.shared_region_size_);
  append_identity(generation_key, main->file.content_identity);
  if (main->file.file_generation) {
    append_generation(generation_key, *main->file.file_generation);
  }
  append_uuid(generation_key, main->file.uuid);
  append_u64(generation_key, main->file.cache_vm_offset);
  for (std::size_t index = 1; index < result.files_.size(); ++index) {
    const auto &file = result.files_[index];
    append_identity(generation_key, file.content_identity);
    if (file.file_generation) {
      append_generation(generation_key, *file.file_generation);
    }
    append_uuid(generation_key, file.uuid);
    append_u64(generation_key, file.cache_vm_offset);
  }
  result.generation_identity_ = sha256(generation_key);
  auto generation = std::make_shared<const DyldSharedCache>(std::move(result));
  generation_cache[cache_key] =
      GenerationCacheEntry{generation,
                           std::vector<DyldCacheFile>(generation->files().begin(),
                                                      generation->files().end())};
  generation->publish_shared_generation_artifact(
      path, options, *main->file.file_generation, main->file.content_identity);
  ++generation_artifact_builds;
  ++generation_builds;
  return generation;
}

DyldSharedCache::ParseStats DyldSharedCache::parse_stats() noexcept {
  std::lock_guard lock{generation_cache_mutex};
  return ParseStats{generation_builds,
                    generation_hits,
                    generation_artifact_builds,
                    generation_artifact_hits,
                    image_builds,
                    image_hits};
}

std::shared_ptr<const MachOImage> DyldSharedCache::parse_image(
    std::uint32_t image_index, ArmArchitectureVersion architecture) const {
  if (image_store_ == nullptr || image_index >= images_.size()) return {};
  const auto architecture_tag = static_cast<std::uint8_t>(architecture);
  const auto key = std::pair{image_index, architecture_tag};
  std::unique_lock generation_lock{generation_cache_mutex};
  std::lock_guard image_lock{image_store_->mutex};
  if (const auto cached = image_store_->images.find(key);
      cached != image_store_->images.end()) {
    ++image_hits;
    return cached->second;
  }

  const auto &image = images_[image_index];
  const DyldCacheRange *header = nullptr;
  for (const auto &range : image.executable_ranges) {
    if (range.address == image.unslid_load_address &&
        range.file_index < files_.size()) {
      header = &range;
      break;
    }
  }
  if (header == nullptr) return {};
  const auto &source = files_[header->file_index];
  try {
    auto parsed = std::make_shared<const MachOImage>(MachOImage::parse(
        source.path, architecture, source.content_identity,
        ImmutableSnapshotKind::RuntimeHot, header->file_offset,
        source.immutable_snapshot, source.file_generation,
        source.immutable_file_view));
    image_store_->images.emplace(key, parsed);
    ++image_builds;
    return parsed;
  } catch (const std::exception &) {
    // Do not publish an incomplete image. A later request may retry against a
    // stable file generation, while existing generations remain untouched.
    return {};
  }
}

std::vector<std::uint32_t> DyldSharedCache::images_intersecting_file_range(
    std::uint32_t file_index, std::uint64_t file_offset,
    std::uint64_t size) const {
  if (size == 0U || file_index >= image_range_index_.size() ||
      add_overflows(file_offset, size)) {
    return {};
  }
  const auto file_end = file_offset + size;
  const auto &entries = image_range_index_[file_index];
  auto first = std::lower_bound(
      entries.begin(), entries.end(), file_offset,
      [](const ImageRangeIndexEntry &entry, std::uint64_t offset) {
        return entry.file_offset < offset;
      });
  while (first != entries.begin() &&
         std::prev(first)->prefix_file_end > file_offset)
    --first;

  std::vector<std::uint32_t> image_indices;
  for (auto iterator = first;
       iterator != entries.end() && iterator->file_offset < file_end;
       ++iterator) {
    if (iterator->file_end <= file_offset) continue;
    image_indices.push_back(iterator->image_index);
  }
  std::sort(image_indices.begin(), image_indices.end());
  image_indices.erase(
      std::unique(image_indices.begin(), image_indices.end()),
      image_indices.end());
  return image_indices;
}

const DyldCacheFile &DyldSharedCache::main_cache() const noexcept {
  return main_cache_;
}

std::span<const DyldCacheFile> DyldSharedCache::files() const noexcept {
  return files_;
}

std::span<const DyldCacheImage> DyldSharedCache::images() const noexcept {
  return images_;
}

const ContentIdentity &DyldSharedCache::generation_identity() const noexcept {
  return generation_identity_;
}

std::uint32_t DyldSharedCache::platform() const noexcept { return platform_; }

std::uint8_t DyldSharedCache::format_version() const noexcept {
  return format_version_;
}

std::uint64_t DyldSharedCache::shared_region_start() const noexcept {
  return shared_region_start_;
}

std::uint64_t DyldSharedCache::shared_region_size() const noexcept {
  return shared_region_size_;
}

std::uint64_t DyldSharedCache::max_slide() const noexcept { return max_slide_; }

const DyldCacheImage *DyldSharedCache::find_image(
    std::string_view install_name) const noexcept {
  const auto image = std::find_if(
      images_.begin(), images_.end(), [&](const auto &candidate) {
        return candidate.path == install_name;
      });
  return image == images_.end() ? nullptr : &*image;
}

} // namespace ilemu
