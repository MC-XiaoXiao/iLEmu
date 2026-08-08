#include "ilemu/address_space.hpp"
#include "ilemu/file_page_cache.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

using ilemu::GuestPageBytes;
using ilemu::guest_memory_page_size;

bool write_page(const std::filesystem::path &path, std::byte first_byte) {
  GuestPageBytes bytes{};
  bytes[0] = first_byte;
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  if (!output) return false;
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "ilemu-file-page-cache-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  if (error) return 1;

  const auto path = root / "firmware.bin";
  if (!write_page(path, std::byte{0x11})) return 1;
  const auto original_modified = std::filesystem::last_write_time(path, error);
  if (error) return 1;

  ilemu::FilePageCache cache;
  const auto first_mapping =
      cache.open_mapping(path, 0, guest_memory_page_size);
  if (!first_mapping) {
    std::cerr << "initial file mapping failed\n";
    return 1;
  }
  const auto first_page = cache.load_page(
      *first_mapping, 0, guest_memory_page_size);
  first_page->materialize();
  if (first_page->bytes[0] != std::byte{0x11}) {
    std::cerr << "initial file page contents failed\n";
    return 1;
  }

  if (!write_page(path, std::byte{0x22})) return 1;
  std::filesystem::last_write_time(path, original_modified, error);
  if (error) {
    std::cerr << "could not restore file timestamp\n";
    return 1;
  }

  const auto second_mapping =
      cache.open_mapping(path, 0, guest_memory_page_size);
  if (!second_mapping ||
      (*first_mapping)->content_identity ==
          (*second_mapping)->content_identity) {
    std::cerr << "content mutation was not observed\n";
    return 1;
  }
  const auto second_page = cache.load_page(
      *second_mapping, 0, guest_memory_page_size);
  second_page->materialize();
  if (second_page == first_page || second_page->bytes[0] != std::byte{0x22}) {
    std::cerr << "stale file page was reused\n";
    return 1;
  }
  if (cache.page_count() != 1U) {
    std::cerr << "obsolete file pages were not invalidated\n";
    return 1;
  }
  const auto repeated_mapping =
      cache.open_mapping(path, 0, guest_memory_page_size);
  const auto identity_stats = cache.stats();
  if (!repeated_mapping || identity_stats.identity_queries != 3U ||
      identity_stats.sha_computations != 2U ||
      identity_stats.sha_bytes != 2U * guest_memory_page_size ||
      identity_stats.identity_hits != 1U ||
      identity_stats.generation_invalidations != 1U) {
    std::cerr << "file identity generation was not reused: "
              << identity_stats.identity_queries << "/"
              << identity_stats.sha_computations << "/"
              << identity_stats.sha_bytes << "/"
              << identity_stats.identity_hits << "/"
              << identity_stats.generation_invalidations << "\n";
    return 1;
  }

  const auto limited_path = root / "limited.bin";
  if (!write_page(limited_path, std::byte{0x2a})) return 1;
  ilemu::FilePageCache limited_cache{{1U}};
  const auto limited_first_mapping =
      limited_cache.open_mapping(path, 0, guest_memory_page_size);
  const auto limited_second_mapping =
      limited_cache.open_mapping(limited_path, 0, guest_memory_page_size);
  if (!limited_first_mapping || !limited_second_mapping) {
    std::cerr << "limited file cache mappings failed\n";
    return 1;
  }
  const auto limited_first_page = limited_cache.load_page(
      *limited_first_mapping, 0, guest_memory_page_size);
  limited_first_page->materialize();
  const auto limited_second_page = limited_cache.load_page(
      *limited_second_mapping, 0, guest_memory_page_size);
  limited_second_page->materialize();
  if (limited_first_page == limited_second_page ||
      limited_cache.page_count() != 1U ||
      limited_second_page->bytes[0] != std::byte{0x2a}) {
    std::cerr << "file page cache LRU limit was not enforced\n";
    return 1;
  }

  const auto rename_path = root / "rename-target.bin";
  const auto replacement_path = root / "rename-target.new";
  if (!write_page(rename_path, std::byte{0x33})) return 1;
  ilemu::AddressSpace renamed_mapping;
  if (!renamed_mapping.map_file(
          0x2000U, guest_memory_page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Execute,
          rename_path, 0)) {
    std::cerr << "atomic-rename mapping failed\n";
    return 1;
  }
  if (!write_page(replacement_path, std::byte{0x44})) return 1;
  std::filesystem::rename(replacement_path, rename_path, error);
  if (error) {
    std::cerr << "atomic-rename replacement failed\n";
    return 1;
  }
  const auto old_mapping_byte = renamed_mapping.read8(
      0x2000U, ilemu::MemoryPermission::Execute);
  if (!old_mapping_byte || *old_mapping_byte != 0x33U) {
    std::cerr << "lazy old mapping followed an atomic rename\n";
    return 1;
  }
  ilemu::AddressSpace new_mapping;
  if (!new_mapping.map_file(
          0x2000U, guest_memory_page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Execute,
          rename_path, 0)) {
    std::cerr << "new atomic-rename mapping failed\n";
    return 1;
  }
  const auto new_mapping_byte =
      new_mapping.read8(0x2000U, ilemu::MemoryPermission::Execute);
  if (!new_mapping_byte || *new_mapping_byte != 0x44U) {
    std::cerr << "new mapping did not observe atomic replacement\n";
    return 1;
  }

  const auto shared_path = root / "shared-rename-target.bin";
  const auto shared_old_alias = root / "shared-rename-old.bin";
  const auto shared_replacement_path = root / "shared-rename-target.new";
  if (!write_page(shared_path, std::byte{0x55})) return 1;
  ilemu::FilePageCache shared_cache;
  const auto shared_file_mapping =
      shared_cache.open_mapping(shared_path, 0, guest_memory_page_size);
  if (!shared_file_mapping) {
    std::cerr << "shared-file mapping setup failed\n";
    return 1;
  }
  const auto shared_page = shared_cache.load_page(
      *shared_file_mapping, 0, guest_memory_page_size);
  const std::array<std::shared_ptr<ilemu::GuestPageBacking>, 1> shared_pages{
      shared_page};
  ilemu::AddressSpace shared_mapping;
  if (!shared_mapping.map_page_backings(
          0x6000U, guest_memory_page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Write,
          shared_pages, ilemu::AddressSpace::PageMappingMode::SharedFile) ||
      !shared_mapping.read8(0x6000U) ||
      !shared_mapping.write8(0x6000U, 0x66U)) {
    std::cerr << "shared-file write setup failed\n";
    return 1;
  }
  std::filesystem::create_hard_link(shared_path, shared_old_alias, error);
  if (error || !write_page(shared_replacement_path, std::byte{0x77})) {
    std::cerr << "shared-file rename fixture setup failed\n";
    return 1;
  }
  std::filesystem::rename(shared_replacement_path, shared_path, error);
  if (error || !shared_mapping.unmap(0x6000U, guest_memory_page_size)) {
    std::cerr << "shared-file rename/writeback failed\n";
    return 1;
  }
  std::ifstream old_file{shared_old_alias, std::ios::binary};
  std::ifstream replacement_file{shared_path, std::ios::binary};
  char old_byte{};
  char replacement_byte{};
  old_file.read(&old_byte, 1);
  replacement_file.read(&replacement_byte, 1);
  if (!old_file || !replacement_file || old_byte != static_cast<char>(0x66) ||
      replacement_byte != static_cast<char>(0x77)) {
    std::cerr << "shared-file writeback followed pathname replacement\n";
    return 1;
  }

  const auto same_content_path = root / "same-content-target.bin";
  const auto same_content_old_alias = root / "same-content-old.bin";
  const auto same_content_replacement = root / "same-content-target.new";
  if (!write_page(same_content_path, std::byte{0x88})) return 1;
  ilemu::FilePageCache generation_cache;
  const auto old_generation_mapping = generation_cache.open_mapping(
      same_content_path, 0, guest_memory_page_size);
  if (!old_generation_mapping) {
    std::cerr << "same-content old mapping setup failed\n";
    return 1;
  }
  const auto old_generation_page = generation_cache.load_page(
      *old_generation_mapping, 0, guest_memory_page_size);
  old_generation_page->materialize();
  std::filesystem::create_hard_link(same_content_path,
                                    same_content_old_alias, error);
  if (error || !write_page(same_content_replacement, std::byte{0x88})) {
    std::cerr << "same-content replacement setup failed\n";
    return 1;
  }
  std::filesystem::rename(same_content_replacement, same_content_path, error);
  if (error) {
    std::cerr << "same-content replacement failed\n";
    return 1;
  }
  const auto new_generation_mapping = generation_cache.open_mapping(
      same_content_path, 0, guest_memory_page_size);
  if (!new_generation_mapping) {
    std::cerr << "same-content new mapping setup failed\n";
    return 1;
  }
  const auto new_generation_page = generation_cache.load_page(
      *new_generation_mapping, 0, guest_memory_page_size);
  if (new_generation_page == old_generation_page) {
    std::cerr << "same-content replacement reused the old file object\n";
    return 1;
  }
  const std::array<std::shared_ptr<ilemu::GuestPageBacking>, 1>
      generation_pages{new_generation_page};
  ilemu::AddressSpace generation_mapping;
  if (!generation_mapping.map_page_backings(
          0x7000U, guest_memory_page_size,
          ilemu::MemoryPermission::Read | ilemu::MemoryPermission::Write,
          generation_pages, ilemu::AddressSpace::PageMappingMode::SharedFile) ||
      !generation_mapping.write8(0x7000U, 0x99U) ||
      !generation_mapping.unmap(0x7000U, guest_memory_page_size)) {
    std::cerr << "same-content generation writeback failed\n";
    return 1;
  }
  std::ifstream same_content_old_file{same_content_old_alias,
                                      std::ios::binary};
  std::ifstream same_content_new_file{same_content_path, std::ios::binary};
  char same_content_old_byte{};
  char same_content_new_byte{};
  same_content_old_file.read(&same_content_old_byte, 1);
  same_content_new_file.read(&same_content_new_byte, 1);
  if (!same_content_old_file || !same_content_new_file ||
      same_content_old_byte != static_cast<char>(0x88) ||
      same_content_new_byte != static_cast<char>(0x99)) {
    std::cerr << "same-content writeback used the old file object\n";
    return 1;
  }

  ilemu::AddressSpace memory;
  if (!memory.map_file(0x4000U, guest_memory_page_size,
                       ilemu::MemoryPermission::Read |
                           ilemu::MemoryPermission::Execute,
                       path, 0) ||
      !memory.is_read_only_executable(0x4000U, sizeof(std::uint32_t)) ||
      !memory.read32(0x4000U, ilemu::MemoryPermission::Execute) ||
      !memory.is_read_only_executable(0x4000U, sizeof(std::uint32_t))) {
    std::cerr << "immutable executable file mapping was not recognized\n";
    return 1;
  }
  const auto first_executable_identity =
      memory.executable_backing_identity(0x4000U, sizeof(std::uint32_t));
  if (!first_executable_identity) {
    std::cerr << "immutable executable identity was not available\n";
    return 1;
  }
  if (!memory.map_file(0x8000U, guest_memory_page_size,
                       ilemu::MemoryPermission::Read |
                           ilemu::MemoryPermission::Execute,
                       path, 0)) {
    std::cerr << "second executable mapping failed\n";
    return 1;
  }
  const auto second_executable_identity =
      memory.executable_backing_identity(0x8000U, sizeof(std::uint32_t));
  if (!second_executable_identity ||
      first_executable_identity->content !=
          second_executable_identity->content ||
      first_executable_identity->layout == second_executable_identity->layout) {
    std::cerr << "content/layout executable identities were not separated\n";
    return 1;
  }
  if (!memory.protect(0x4000U, guest_memory_page_size,
                      ilemu::MemoryPermission::Read |
                          ilemu::MemoryPermission::Write |
                          ilemu::MemoryPermission::Execute) ||
      memory.is_read_only_executable(0x4000U, sizeof(std::uint32_t))) {
    std::cerr << "writable executable mapping was marked immutable\n";
    return 1;
  }

  ilemu::AddressSpace mutated_memory;
  if (!mutated_memory.map_file(0xc000U, guest_memory_page_size,
                               ilemu::MemoryPermission::Read |
                                   ilemu::MemoryPermission::Execute,
                               path, 0) ||
      !mutated_memory.read32(0xc000U, ilemu::MemoryPermission::Execute) ||
      !mutated_memory.protect(0xc000U, guest_memory_page_size,
                              ilemu::MemoryPermission::Read |
                                  ilemu::MemoryPermission::Write) ||
      !mutated_memory.write32(0xc000U, 0xe1a00000U) ||
      !mutated_memory.protect(0xc000U, guest_memory_page_size,
                              ilemu::MemoryPermission::Read |
                                  ilemu::MemoryPermission::Execute) ||
      mutated_memory.executable_backing_identity(0xc000U,
                                                 sizeof(std::uint32_t))) {
    std::cerr << "mutated executable backing was reused as immutable\n";
    return 1;
  }

  std::filesystem::remove_all(root, error);
  return 0;
}
