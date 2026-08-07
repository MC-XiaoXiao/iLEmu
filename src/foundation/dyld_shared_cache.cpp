#include "ilemu/dyld_shared_cache.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ilemu {
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
  const auto content_identity = sha256_file(path);
  if (!content_identity) return std::nullopt;

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

} // namespace

std::optional<DyldSharedCache>
DyldSharedCache::parse(const std::filesystem::path &path,
                       DyldSharedCacheOptions options) {
  const auto main = parse_file(path, 0, 0, {}, true);
  if (!main) return std::nullopt;

  std::ifstream header_input{path, std::ios::binary};
  if (!header_input) return std::nullopt;
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
    return std::nullopt;
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
    if (!offset || !count) return std::nullopt;
    subcache_offset = *offset;
    subcache_count = *count;
  }
  if (subcache_count > maximum_subcache_count) return std::nullopt;
  const bool has_extended_subcache_entry =
      field_available(main->mapping_offset, cache_subtype_field,
                      sizeof(std::uint32_t));
  const auto subcache_entry_size =
      has_extended_subcache_entry ? subcache_size : subcache_v1_size;
  if (subcache_count != 0U &&
      !table_fits(subcache_offset, subcache_count, subcache_entry_size,
                  main->file.file_size, maximum_subcache_count)) {
    return std::nullopt;
  }
  if (!options.subcache_paths.empty() &&
      options.subcache_paths.size() != subcache_count) {
    return std::nullopt;
  }

  for (std::uint32_t index = 0; index < subcache_count; ++index) {
    const auto entry_offset = static_cast<std::uint64_t>(subcache_offset) +
                              static_cast<std::uint64_t>(index) *
                                  subcache_entry_size;
    const auto uuid = read_uuid(header_input, entry_offset);
    const auto vm_offset = read_u64(header_input, entry_offset + 16U);
    if (!uuid || !vm_offset) return std::nullopt;
    std::string suffix;
    if (has_extended_subcache_entry) {
      const auto suffix_bytes = read_blob(header_input, entry_offset + 24U, 32U);
      if (!suffix_bytes) return std::nullopt;
      const auto end = std::find(suffix_bytes->begin(), suffix_bytes->end(),
                                 std::byte{});
      suffix.assign(reinterpret_cast<const char *>(suffix_bytes->data()),
                    static_cast<std::size_t>(end - suffix_bytes->begin()));
      if (suffix.find('/') != std::string::npos ||
          suffix.find('\\') != std::string::npos) {
        return std::nullopt;
      }
    }
    const auto subcache_path = options.subcache_paths.empty()
                                   ? discover_subcache(path, suffix, index)
                                   : options.subcache_paths[index];
    const auto subcache =
        parse_file(subcache_path, index + 1U, *vm_offset, suffix, false);
    if (!subcache || subcache->file.uuid != *uuid) return std::nullopt;
    result.files_.push_back(subcache->file);
  }

  std::uint32_t image_offset = 0;
  std::uint32_t image_count = 0;
  if (field_available(main->mapping_offset, images_count_field,
                      sizeof(std::uint32_t))) {
    const auto offset = read_u32(header_input, images_offset_field);
    const auto count = read_u32(header_input, images_count_field);
    if (!offset || !count) return std::nullopt;
    image_offset = *offset;
    image_count = *count;
  } else {
    const auto offset = read_u32(header_input, images_offset_old_field);
    const auto count = read_u32(header_input, images_count_old_field);
    if (!offset || !count) return std::nullopt;
    image_offset = *offset;
    image_count = *count;
  }
  if (!table_fits(image_offset, image_count, image_info_size,
                  main->file.file_size, maximum_image_count)) {
    return std::nullopt;
  }
  result.images_.reserve(image_count);
  for (std::uint32_t index = 0; index < image_count; ++index) {
    const auto offset = static_cast<std::uint64_t>(image_offset) +
                        static_cast<std::uint64_t>(index) * image_info_size;
    const auto load_address = read_u64(header_input, offset);
    const auto path_offset = read_u32(header_input, offset + 24U);
    if (!load_address || !path_offset) return std::nullopt;
    const auto image_path =
        read_string(header_input, *path_offset, main->file.file_size);
    if (!image_path) return std::nullopt;
    result.images_.push_back(
        DyldCacheImage{index, *image_path, *load_address});
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
      return std::nullopt;
    }
    const auto file_offset =
        mapping->file_offset + (info.load_address - mapping->address);
    image->executable_ranges.push_back(
        DyldCacheRange{info.load_address, size, file_offset,
                       mapping->file_index, mapping->initial_protection,
                       mapping->maximum_protection});
    const auto identity = sha256_file(result.files_[mapping->file_index].path,
                                      file_offset, size);
    if (!identity) return std::nullopt;
    image->text_identity = *identity;
  }

  std::vector<std::byte> generation_key;
  generation_key.reserve(96U + result.files_.size() * 64U +
                         options.architecture.size());
  append_u32(generation_key, DyldSharedCache::parser_schema_version);
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
  append_uuid(generation_key, main->file.uuid);
  append_u64(generation_key, main->file.cache_vm_offset);
  for (std::size_t index = 1; index < result.files_.size(); ++index) {
    const auto &file = result.files_[index];
    append_identity(generation_key, file.content_identity);
    append_uuid(generation_key, file.uuid);
    append_u64(generation_key, file.cache_vm_offset);
  }
  result.generation_identity_ = sha256(generation_key);
  return result;
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
