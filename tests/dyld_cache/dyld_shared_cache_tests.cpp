#include "ilemu/dyld_shared_cache.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void write_le32(std::vector<std::byte> &bytes, std::size_t offset,
                std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
  }
}

void write_le64(std::vector<std::byte> &bytes, std::size_t offset,
                std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
  }
}

void write_uuid(std::vector<std::byte> &bytes, std::size_t offset,
                std::uint8_t seed) {
  for (std::size_t index = 0; index < 16U; ++index) {
    bytes[offset + index] = static_cast<std::byte>(seed + index);
  }
}

void write_string(std::vector<std::byte> &bytes, std::size_t offset,
                  std::string_view value) {
  for (std::size_t index = 0; index < value.size(); ++index) {
    bytes[offset + index] = static_cast<std::byte>(value[index]);
  }
}

bool write_file(const std::filesystem::path &path,
                const std::vector<std::byte> &bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) return false;
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

std::vector<std::byte> make_subcache() {
  std::vector<std::byte> bytes(0x3000U);
  write_string(bytes, 0, "dyld_v1  armv7");
  write_le32(bytes, 16, 0x100U);
  write_le32(bytes, 20, 1U);
  write_uuid(bytes, 88, 0x20U);
  write_le64(bytes, 0x100U, 0x30001000U);
  write_le64(bytes, 0x108U, 0x1000U);
  write_le64(bytes, 0x110U, 0x1000U);
  write_le32(bytes, 0x118U, 5U);
  write_le32(bytes, 0x11cU, 5U);
  bytes[0x1000U] = std::byte{0x5a};
  return bytes;
}

std::vector<std::byte> make_main_cache() {
  std::vector<std::byte> bytes(0x5000U);
  write_string(bytes, 0, "dyld_v1  armv7");
  write_le32(bytes, 16, 0x400U);
  write_le32(bytes, 20, 1U);
  write_uuid(bytes, 88, 0x10U);
  write_le32(bytes, 216, 2U);
  write_le32(bytes, 220, 3U);
  write_le64(bytes, 224, 0x30000000U);
  write_le64(bytes, 232, 0x2000U);
  write_le64(bytes, 240, 0x1000U);
  write_le32(bytes, 312, 0x800U);
  write_le32(bytes, 316, 1U);
  write_le64(bytes, 136, 0x1200U);
  write_le64(bytes, 144, 1U);
  write_le32(bytes, 392, 0x600U);
  write_le32(bytes, 396, 1U);
  write_le32(bytes, 448, 0x1000U);
  write_le32(bytes, 452, 1U);
  write_le32(bytes, 456, 1U);

  write_uuid(bytes, 0x600U, 0x20U);
  write_le64(bytes, 0x610U, 0x1000U);
  write_string(bytes, 0x618U, ".01");

  write_le64(bytes, 0x800U, 0x30000000U);
  write_le64(bytes, 0x808U, 0x1000U);
  write_le64(bytes, 0x810U, 0x2000U);
  write_le64(bytes, 0x818U, 0x900U);
  write_le64(bytes, 0x820U, 4U);
  write_le64(bytes, 0x828U, 0x20U);
  write_le32(bytes, 0x830U, 5U);
  write_le32(bytes, 0x834U, 5U);
  write_le32(bytes, 0x900U, 4U);

  write_le64(bytes, 0x1000U, 0x30000000U);
  write_le32(bytes, 0x1018U, 0x1300U);
  write_uuid(bytes, 0x1200U, 0x30U);
  write_le64(bytes, 0x1210U, 0x30000000U);
  write_le32(bytes, 0x1218U, 0x1000U);
  write_le32(bytes, 0x121cU, 0x1300U);
  write_string(bytes, 0x1300U, "/usr/lib/libFoo.dylib");
  std::fill(bytes.begin() + 0x2000U, bytes.begin() + 0x3000U,
            std::byte{0xa5});
  return bytes;
}

bool require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "ilemu-dyld-shared-cache-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  if (error) return 1;

  const auto main_path = root / "dyld_shared_cache_armv7";
  const auto subcache_path = root / "dyld_shared_cache_armv7.01";
  const auto main_bytes = make_main_cache();
  if (!write_file(main_path, main_bytes) ||
      !write_file(subcache_path, make_subcache())) {
    return 1;
  }

  const auto first = ilemu::DyldSharedCache::parse(main_path);
  if (!require(first.has_value(), "valid shared cache was rejected")) return 1;
  if (!require(first->files().size() == 2U,
               "split cache did not load both files") ||
      !require(first->images().size() == 1U,
               "cache image table was not parsed") ||
      !require(first->platform() == 2U && first->format_version() == 3U,
               "cache platform/version fields were not parsed") ||
      !require(first->main_cache().mappings.size() == 1U &&
                   first->main_cache().mappings.front().slide_info_version == 4U,
               "mapping-with-slide metadata was not parsed")) {
    return 1;
  }
  const auto *image = first->find_image("/usr/lib/libFoo.dylib");
  if (!require(image != nullptr && image->text_uuid.has_value() &&
                   image->executable_ranges.size() == 1U &&
                   image->text_identity.has_value(),
               "image text metadata was not correlated")) {
    return 1;
  }
  const auto first_generation = first->generation_identity();

  auto changed_main = main_bytes;
  changed_main[0x2000U] = std::byte{0x5b};
  if (!write_file(main_path, changed_main)) return 1;
  const auto second = ilemu::DyldSharedCache::parse(main_path);
  if (!require(second.has_value() &&
                   second->generation_identity() != first_generation,
               "cache content change did not create a new generation")) {
    return 1;
  }

  std::filesystem::remove(subcache_path, error);
  if (!require(!ilemu::DyldSharedCache::parse(main_path).has_value(),
               "missing subcache was accepted")) {
    return 1;
  }

  const auto truncated_path = root / "truncated-cache";
  if (!write_file(truncated_path, std::vector<std::byte>(32U)) ||
      !require(!ilemu::DyldSharedCache::parse(truncated_path).has_value(),
               "truncated cache was accepted")) {
    return 1;
  }

  const auto invalid_count_path = root / "invalid-count-cache";
  auto invalid_count = main_bytes;
  write_le32(invalid_count, 20, 0xffff'ffffU);
  if (!write_file(invalid_count_path, invalid_count) ||
      !require(!ilemu::DyldSharedCache::parse(invalid_count_path).has_value(),
               "overflowing mapping count was accepted")) {
    return 1;
  }

  std::filesystem::remove_all(root, error);
  return 0;
}
