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

  std::filesystem::remove_all(root, error);
  return 0;
}
