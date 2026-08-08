#include "ilemu/executable_catalog.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
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

void write_be32(std::vector<std::byte> &bytes, std::size_t offset,
                std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<std::byte>(
        value >> ((sizeof(value) - index - 1U) * 8U));
  }
}

std::vector<std::byte> minimal_macho(std::uint32_t file_type,
                                     std::uint8_t uuid_seed) {
  std::vector<std::byte> bytes(52U);
  write_le32(bytes, 0, 0xfeedfaceU);
  write_le32(bytes, 4, 12U);
  write_le32(bytes, 8, 6U);
  write_le32(bytes, 12, file_type);
  write_le32(bytes, 16, 1U);
  write_le32(bytes, 20, 24U);
  write_le32(bytes, 24, 0U);
  write_le32(bytes, 28, 0x1bU);
  write_le32(bytes, 32, 24U);
  for (std::size_t index = 0; index < 16U; ++index) {
    bytes[36U + index] = static_cast<std::byte>(uuid_seed + index);
  }
  return bytes;
}

std::vector<std::byte> minimal_shared_cache() {
  std::vector<std::byte> bytes(0x2000U);
  const std::string magic{"dyld_v1  armv7"};
  std::copy(magic.begin(), magic.end(),
            reinterpret_cast<char *>(bytes.data()));
  write_le32(bytes, 16U, 0x100U);
  write_le32(bytes, 20U, 1U);
  write_le64(bytes, 0x100U, 0x30000000U);
  write_le64(bytes, 0x108U, 0x1000U);
  write_le64(bytes, 0x110U, 0x1000U);
  write_le32(bytes, 0x118U, 5U);
  write_le32(bytes, 0x11cU, 5U);
  return bytes;
}

void write_file(const std::filesystem::path &path,
                std::span<const std::byte> bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main() {
  const auto abc = std::string{"abc"};
  const auto abc_identity = ilemu::sha256(std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(abc.data()), abc.size()});
  if (abc_identity.hex() !=
      "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
    std::cerr << "SHA-256 known vector failed\n";
    return 1;
  }

  const auto root = std::filesystem::temp_directory_path() /
                    "ilemu-executable-catalog-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root / "Foo.framework", error);
  if (error) return 1;
  const auto framework_path = root / "Foo.framework" / "Foo";
  const auto framework_bytes = minimal_macho(6U, 0x10U);
  write_file(framework_path, framework_bytes);

  auto image = ilemu::MachOImage::parse(framework_path);
  if (image.content_identity() !=
          ilemu::sha256(std::span<const std::byte>{framework_bytes}) ||
      !image.uuid() || image.uuid()->front() != std::byte{0x10}) {
    std::cerr << "Mach-O content identity or UUID failed\n";
    return 1;
  }
  ilemu::ContentIdentity supplied_identity;
  supplied_identity.digest.fill(std::byte{0x5a});
  const auto supplied_image = ilemu::MachOImage::parse(
      framework_path, ilemu::ArmArchitectureVersion::Armv6K,
      supplied_identity);
  if (supplied_image.content_identity() != supplied_identity ||
      supplied_image.file_size() != image.file_size() ||
      supplied_image.uuid() != image.uuid()) {
    std::cerr << "Mach-O supplied identity reuse failed\n";
    return 1;
  }
  ilemu::ExecutableCatalog catalog;
  const auto &entry = catalog.register_image(image);
  if (!entry.kinds.contains(ilemu::ExecutableCatalogKind::Framework) ||
      catalog.find(image.content_identity()) != &entry) {
    std::cerr << "framework catalog classification failed\n";
    return 1;
  }

  auto fat_bytes = std::vector<std::byte>(0x240U);
  write_be32(fat_bytes, 0, 0xcafebabeU);
  write_be32(fat_bytes, 4, 2U);
  write_be32(fat_bytes, 8, 12U);
  write_be32(fat_bytes, 12, 6U);
  write_be32(fat_bytes, 16, 0x100U);
  write_be32(fat_bytes, 20, 52U);
  write_be32(fat_bytes, 28, 12U);
  write_be32(fat_bytes, 32, 9U);
  write_be32(fat_bytes, 36, 0x200U);
  write_be32(fat_bytes, 40, 52U);
  const auto armv6 = minimal_macho(2U, 0x20U);
  const auto armv7 = minimal_macho(2U, 0x30U);
  std::copy(armv6.begin(), armv6.end(), fat_bytes.begin() + 0x100);
  std::copy(armv7.begin(), armv7.end(), fat_bytes.begin() + 0x200);
  const auto fat_path = root / "FatBinary";
  write_file(fat_path, fat_bytes);
  const auto fat_image = ilemu::MachOImage::parse(
      fat_path, ilemu::ArmArchitectureVersion::Armv7);
  if (!fat_image.fat_container() || fat_image.uuid()->front() != std::byte{0x30}) {
    std::cerr << "FAT slice selection failed\n";
    return 1;
  }
  const auto &mapping = catalog.register_mapping(fat_path, 0x200U, 52U);
  if (!mapping.kinds.contains(ilemu::ExecutableCatalogKind::DynamicMapping) ||
      mapping.mappings.size() != 1U) {
    std::cerr << "dynamic mapping catalog failed\n";
    return 1;
  }
  const auto cache_path = root / "dyld_shared_cache_armv7";
  const auto cache_bytes = minimal_shared_cache();
  write_file(cache_path, cache_bytes);
  ilemu::ExecutableCatalog scanned_catalog;
  const auto scan = scanned_catalog.register_tree(root);
  if (scan.regular_files != 3U || scan.mach_o_images != 2U ||
      scan.dyld_shared_cache_generations != 1U ||
      scan.dyld_shared_cache_images != 0U ||
      scan.failed_files != 0U || scanned_catalog.size() != 2U) {
    std::cerr << "firmware catalog tree scan failed\n";
    return 1;
  }
  const auto *catalogued_framework =
      scanned_catalog.find_path(framework_path);
  if (catalogued_framework == nullptr ||
      catalogued_framework->file_size != framework_bytes.size()) {
    std::cerr << "catalog path lookup or generation size failed\n";
    return 1;
  }
  const auto manifest_path = root / "catalog.bin";
  if (!scanned_catalog.save(manifest_path) ||
      !scanned_catalog.save(manifest_path)) {
    std::cerr << "catalog manifest save failed\n";
    return 1;
  }
  ilemu::ExecutableCatalog reloaded_catalog;
  if (!reloaded_catalog.load(manifest_path) ||
      reloaded_catalog.size() != scanned_catalog.size() ||
      reloaded_catalog.find(image.content_identity()) == nullptr) {
    std::cerr << "catalog manifest round-trip failed\n";
    return 1;
  }
  const auto corrupt_manifest = root / "catalog-corrupt.bin";
  const std::vector<std::byte> corrupt_bytes(8U, std::byte{0});
  write_file(corrupt_manifest, corrupt_bytes);
  const auto retained_entries = reloaded_catalog.size();
  if (reloaded_catalog.load(corrupt_manifest) ||
      reloaded_catalog.size() != retained_entries) {
    std::cerr << "corrupt catalog manifest was not rejected safely\n";
    return 1;
  }
  std::filesystem::remove_all(root, error);
  return 0;
}
