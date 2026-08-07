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
  std::filesystem::remove_all(root, error);
  return 0;
}
