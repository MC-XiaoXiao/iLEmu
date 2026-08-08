#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ilemu/content_identity.hpp"

namespace ilemu {

inline constexpr std::uint32_t guest_memory_page_size = 4096;
inline constexpr std::size_t guest_file_prefetch_pages = 32;
using GuestPageBytes = std::array<std::byte, guest_memory_page_size>;

struct GuestFileIoState {
  mutable std::mutex mutex;
  mutable std::shared_ptr<std::ifstream> stream;
  mutable std::map<std::uint64_t, GuestPageBytes> prefetched_pages;
};

struct GuestFileBacking {
  GuestFileBacking(std::filesystem::path file_path,
                   std::uint64_t mapping_start,
                   std::uint64_t mapping_end)
      : path(std::move(file_path)),
        io_state{std::make_shared<GuestFileIoState>()},
        first_offset(mapping_start), end_offset(mapping_end) {}

  std::filesystem::path path;
  std::string cache_path;
  std::uintmax_t file_size{};
  std::filesystem::file_time_type modified;
  ContentIdentity content_identity;
  // The stream is opened when the mapping is created and shared by range
  // splits. This preserves the old vnode/file object across atomic rename.
  std::shared_ptr<GuestFileIoState> io_state;
  std::uint64_t first_offset{};
  std::uint64_t end_offset{};
};

struct GuestPageBacking {
  mutable GuestPageBytes bytes{};

  GuestPageBacking() = default;
  GuestPageBacking(const GuestPageBacking &other);
  GuestPageBacking &operator=(const GuestPageBacking &) = delete;

  // File-backed mappings are materialized on first guest access. Anonymous
  // and IPC-backed pages have no source and remain ordinary byte arrays.
  void materialize() const;
  // A consumer of a shared mapping can opt its physical page into a common
  // write generation. Later aliases then retain cross-address-space dirty
  // visibility without observing or copying each individual store.
  [[nodiscard]] bool enable_shared_write_tracking();
  [[nodiscard]] bool shared_write_tracking_enabled() const;
  [[nodiscard]] static std::uint64_t shared_write_tracking_epoch();
  [[nodiscard]] std::uint64_t shared_write_generation() const;
  void mark_shared_write();
  [[nodiscard]] bool file_backed() const;
  // Persists a MAP_SHARED page without affecting private file mappings.
  [[nodiscard]] bool flush_file();

private:
  friend class FilePageCache;

  mutable std::mutex mutex_;
  mutable std::shared_ptr<GuestFileBacking> file_backing_;
  std::shared_ptr<GuestFileBacking> file_writeback_;
  std::uint64_t file_offset_{};
  std::uint32_t file_byte_count_{};
  // Set before this page is published and never changed afterwards. This
  // avoids taking the page lock for anonymous and already-private pages.
  bool has_file_source_{};
  std::atomic<bool> shared_write_tracking_{};
  std::atomic<std::uint64_t> shared_write_generation_{};
};

// Process-family cache for immutable firmware file pages. AddressSpace keeps a
// strong reference to the cache across fork, while a private write detaches the
// corresponding GuestPageBacking through its normal copy-on-write path.
struct FilePageCacheLimits {
  // Zero means unbounded. Eviction only drops the cache's reference; an
  // AddressSpace that still maps a page keeps the backing alive.
  std::size_t maximum_pages{32U * 1024U};
};

class FilePageCache {
public:
  explicit FilePageCache(FilePageCacheLimits limits = {})
      : limits_{limits} {}

  // Validates a file-backed range and records the immutable identity used by
  // later page faults. No per-page objects or file contents are created here.
  [[nodiscard]] std::optional<std::shared_ptr<GuestFileBacking>>
  open_mapping(const std::filesystem::path &path, std::uint64_t file_offset,
               std::uint32_t size);

  // Creates or reuses one page for an already validated mapping. The page
  // remains byte-lazy; GuestPageBacking::materialize performs clustered I/O.
  [[nodiscard]] std::shared_ptr<GuestPageBacking>
  load_page(const std::shared_ptr<GuestFileBacking> &mapping,
            std::uint64_t file_offset, std::uint32_t byte_count);

  [[nodiscard]] std::optional<std::vector<std::shared_ptr<GuestPageBacking>>>
  load_pages(const std::filesystem::path &path, std::uint64_t file_offset,
             std::uint32_t size);

  [[nodiscard]] std::size_t page_count() const;

private:
  struct Identity {
    std::uintmax_t file_size{};
    std::filesystem::file_time_type modified;
    ContentIdentity content_identity;
  };

  struct Key {
    std::string path;
    ContentIdentity content_identity;
    std::uint64_t file_offset{};
    std::uint32_t byte_count{};

    [[nodiscard]] bool operator<(const Key &other) const;
  };

  struct PageRecord {
    std::shared_ptr<GuestPageBacking> page;
    std::list<Key>::iterator lru_position;
  };

  void touch_locked(std::map<Key, PageRecord>::iterator iterator);
  void erase_path_locked(const std::string &path);
  void evict_locked();

  mutable std::mutex mutex_;
  FilePageCacheLimits limits_;
  std::map<std::string, Identity> identities_;
  std::map<Key, PageRecord> pages_;
  std::list<Key> lru_;
};

} // namespace ilemu
