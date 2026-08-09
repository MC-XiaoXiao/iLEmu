#include "ilemu/file_page_cache.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <iterator>
#include <limits>
#include <tuple>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ilemu {
namespace {

constexpr std::uint64_t file_prefetch_bytes =
    guest_file_prefetch_pages * guest_memory_page_size;

std::atomic<std::uint64_t> global_shared_write_tracking_epoch{};
std::atomic<std::uint64_t> next_reservation_identity{1};

[[nodiscard]] int open_file_descriptor(const std::filesystem::path &path) {
  auto descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (descriptor < 0) {
    descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  }
  return descriptor;
}

[[nodiscard]] std::filesystem::file_time_type file_time_from_stat(
    const struct stat &file_stat) {
  using namespace std::chrono;
  const auto duration = seconds{file_stat.st_mtim.tv_sec} +
                        nanoseconds{file_stat.st_mtim.tv_nsec};
  return std::filesystem::file_time_type{
      duration_cast<std::filesystem::file_time_type::duration>(duration)};
}

[[nodiscard]] GuestFileGeneration generation_from_stat(
    const struct stat &file_stat) {
  return GuestFileGeneration{
      static_cast<std::uint64_t>(file_stat.st_dev),
      static_cast<std::uint64_t>(file_stat.st_ino),
      static_cast<std::uint64_t>(file_stat.st_size),
      static_cast<std::int64_t>(file_stat.st_mtim.tv_sec),
      static_cast<std::int64_t>(file_stat.st_mtim.tv_nsec),
      static_cast<std::int64_t>(file_stat.st_ctim.tv_sec),
      static_cast<std::int64_t>(file_stat.st_ctim.tv_nsec)};
}

[[nodiscard]] bool same_file_stat(const struct stat &first,
                                  const struct stat &second) {
  return generation_from_stat(first) == generation_from_stat(second);
}

[[nodiscard]] std::string stable_path(const std::filesystem::path &path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  return (error ? path.lexically_normal() : canonical).string();
}

struct GlobalIdentityRecord {
  GuestFileGeneration generation;
  ContentIdentity content_identity;
  std::weak_ptr<GuestFileIoState> io_state;
};

std::mutex global_identity_mutex;
std::map<std::string, GlobalIdentityRecord> global_identities;

} // namespace

GuestFileIoState::~GuestFileIoState() {
  if (file_descriptor >= 0) {
    static_cast<void>(::close(file_descriptor));
  }
}

GuestPageBacking::GuestPageBacking()
    : reservation_identity_{next_reservation_identity.fetch_add(
          1, std::memory_order_relaxed)} {}

GuestPageBacking::GuestPageBacking(const GuestPageBacking &other) {
  other.materialize();
  const std::scoped_lock lock{other.mutex_};
  bytes = other.bytes;
  reservation_identity_.store(
      next_reservation_identity.fetch_add(1, std::memory_order_relaxed),
      std::memory_order_release);
}

void GuestPageBacking::invalidate_reservation_identity() noexcept {
  reservation_identity_.store(
      next_reservation_identity.fetch_add(1, std::memory_order_relaxed),
      std::memory_order_release);
}

void GuestPageBacking::materialize() const {
  if (!has_file_source_) return;
  const std::scoped_lock page_lock{mutex_};
  if (!file_backing_) return;

  const auto file = file_backing_;
  const auto io_state = file->io_state;
  const std::scoped_lock file_lock{io_state->mutex};
  std::fill(bytes.begin(), bytes.end(), std::byte{});
  if (const auto prefetched_page =
          io_state->prefetched_pages.find(file_offset_);
      prefetched_page != io_state->prefetched_pages.end()) {
    bytes = prefetched_page->second;
    io_state->prefetched_pages.erase(prefetched_page);
  } else {
    const auto aligned_start = file_offset_ & ~(file_prefetch_bytes - 1U);
    const auto read_start = std::max(file->first_offset, aligned_start);
    const auto read_size = std::min(file->end_offset - read_start,
                                    file_prefetch_bytes);
    const auto read_pages = static_cast<std::size_t>(
        (read_size + guest_memory_page_size - 1U) /
        guest_memory_page_size);
    if (io_state->file_descriptor >= 0 &&
        read_start <=
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
      std::vector<std::byte> batch(static_cast<std::size_t>(read_size));
      std::size_t received = 0;
      while (received < batch.size()) {
        ssize_t count = -1;
        do {
          count = ::pread(
              io_state->file_descriptor, batch.data() + received,
              batch.size() - received,
              static_cast<off_t>(read_start + received));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) break;
        received += static_cast<std::size_t>(count);
      }
      for (std::size_t index = 0; index < read_pages; ++index) {
        const auto offset = index * guest_memory_page_size;
        GuestPageBytes page_bytes{};
        if (offset < batch.size()) {
          const auto count = std::min<std::size_t>(
              guest_memory_page_size, batch.size() - offset);
          std::copy_n(batch.begin() + static_cast<std::ptrdiff_t>(offset),
                      count, page_bytes.begin());
        }
        const auto page_offset =
            read_start + static_cast<std::uint64_t>(offset);
        if (page_offset == file_offset_) {
          bytes = page_bytes;
        } else if (received > offset) {
          io_state->prefetched_pages.emplace(page_offset,
                                             std::move(page_bytes));
        }
      }
    }
  }
  file_backing_.reset();
}

bool GuestPageBacking::enable_shared_write_tracking() {
  bool expected = false;
  if (!shared_write_tracking_.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  static_cast<void>(global_shared_write_tracking_epoch.fetch_add(
      1, std::memory_order_release));
  return true;
}

bool GuestPageBacking::shared_write_tracking_enabled() const {
  return shared_write_tracking_.load(std::memory_order_acquire);
}

std::uint64_t GuestPageBacking::shared_write_tracking_epoch() {
  return global_shared_write_tracking_epoch.load(std::memory_order_acquire);
}

std::uint64_t GuestPageBacking::shared_write_generation() const {
  return shared_write_generation_.load(std::memory_order_acquire);
}

void GuestPageBacking::mark_shared_write() {
  if (!shared_write_tracking_enabled())
    return;
  static_cast<void>(
      shared_write_generation_.fetch_add(1, std::memory_order_release));
}

bool GuestPageBacking::file_backed() const {
  return static_cast<bool>(file_writeback_);
}

bool GuestPageBacking::flush_file() {
  if (!file_writeback_) return true;

  GuestPageBytes snapshot{};
  {
    const std::scoped_lock page_lock{mutex_};
    snapshot = bytes;
  }

  const auto file = file_writeback_;
  const auto io_state = file->io_state;
  const std::scoped_lock file_lock{io_state->mutex};
  const auto descriptor = io_state->file_descriptor;
  if (descriptor < 0) return false;

  std::size_t written = 0;
  while (written < file_byte_count_) {
    const auto result = ::pwrite(
        descriptor,
        reinterpret_cast<const char *>(snapshot.data()) + written,
        file_byte_count_ - written,
        static_cast<off_t>(file_offset_ + written));
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      break;
    written += static_cast<std::size_t>(result);
  }
  return written == file_byte_count_;
}

bool FilePageCache::Key::operator<(const Key &other) const {
  return std::tie(path, generation, content_identity, file_offset, byte_count) <
         std::tie(other.path, other.generation, other.content_identity,
                  other.file_offset, other.byte_count);
}

void FilePageCache::touch_locked(
    std::map<Key, PageRecord>::iterator iterator) {
  lru_.splice(lru_.end(), lru_, iterator->second.lru_position);
  iterator->second.lru_position = std::prev(lru_.end());
}

void FilePageCache::erase_path_locked(const std::string &path) {
  for (auto page = pages_.begin(); page != pages_.end();) {
    if (page->first.path != path) {
      ++page;
      continue;
    }
    lru_.erase(page->second.lru_position);
    page = pages_.erase(page);
  }
}

void FilePageCache::evict_locked() {
  if (limits_.maximum_pages == 0U) return;
  while (pages_.size() > limits_.maximum_pages && !lru_.empty()) {
    const auto key = lru_.front();
    lru_.pop_front();
    const auto page = pages_.find(key);
    if (page != pages_.end()) pages_.erase(page);
  }
}

std::optional<std::shared_ptr<GuestFileBacking>>
FilePageCache::open_mapping(const std::filesystem::path &path,
                            std::uint64_t file_offset,
                            std::uint32_t size,
                            std::optional<GuestFileGeneration>
                                expected_generation,
                            std::optional<ContentIdentity>
                                expected_content_identity) {
  if (size == 0 || file_offset % guest_memory_page_size != 0) {
    return std::nullopt;
  }

  auto descriptor = open_file_descriptor(path);
  if (descriptor < 0) return std::nullopt;
  const auto close_descriptor = [&]() {
    if (descriptor >= 0) {
      static_cast<void>(::close(descriptor));
      descriptor = -1;
    }
  };

  struct stat file_stat {};
  if (::fstat(descriptor, &file_stat) != 0 || !S_ISREG(file_stat.st_mode) ||
      file_stat.st_size < 0) {
    close_descriptor();
    return std::nullopt;
  }
  const auto file_size = static_cast<std::uintmax_t>(file_stat.st_size);
  const auto generation = generation_from_stat(file_stat);
  if (expected_generation && *expected_generation != generation) {
    close_descriptor();
    return std::nullopt;
  }
  if (file_offset > file_size) {
    close_descriptor();
    return std::nullopt;
  }
  const auto available = file_size - file_offset;
  const auto page_rounded_available =
      (available + guest_memory_page_size - 1U) &
      ~(static_cast<std::uintmax_t>(guest_memory_page_size) - 1U);
  if (size > page_rounded_available) {
    close_descriptor();
    return std::nullopt;
  }
  const auto modified = file_time_from_stat(file_stat);
  const auto normalized_path = stable_path(path);
  std::optional<ContentIdentity> content_identity;
  {
    const std::scoped_lock lock{mutex_};
    ++stats_.identity_queries;
    const auto identity = identities_.find(normalized_path);
    if (identity != identities_.end() &&
        identity->second.generation == generation) {
      content_identity = identity->second.content_identity;
      ++stats_.identity_hits;
    } else {
      if (identity != identities_.end()) {
        erase_path_locked(normalized_path);
        ++stats_.generation_invalidations;
      }
    }
  }
  if (!content_identity) {
    bool computed = false;
    {
      const std::scoped_lock lock{global_identity_mutex};
      const auto identity = global_identities.find(normalized_path);
      if (identity != global_identities.end() &&
          identity->second.generation == generation) {
        content_identity = identity->second.content_identity;
      } else {
        computed = true;
          content_identity = sha256_file(descriptor);
          if (content_identity) {
            global_identities[normalized_path] =
              GlobalIdentityRecord{generation, *content_identity, {}};
          }
      }
    }
    if (!content_identity) {
      close_descriptor();
      return std::nullopt;
    }
    const std::scoped_lock lock{mutex_};
    if (computed) {
      ++stats_.sha_computations;
      stats_.sha_bytes += static_cast<std::uint64_t>(file_size);
    } else {
      ++stats_.identity_hits;
    }
    identities_[normalized_path] = Identity{generation, *content_identity};
  }
  struct stat final_file_stat {};
  if (::fstat(descriptor, &final_file_stat) != 0 ||
      !same_file_stat(file_stat, final_file_stat)) {
    close_descriptor();
    return std::nullopt;
  }
  if (expected_content_identity &&
      *expected_content_identity != *content_identity) {
    close_descriptor();
    return std::nullopt;
  }

  // Every immutable executable mapping used to retain its own descriptor.
  // Firmware fork fan-out maps the same binaries into many AddressSpaces and
  // can therefore exhaust a normal host RLIMIT_NOFILE even though only a
  // modest number of distinct vnodes are in use. Share the descriptor for an
  // unchanged canonical path/generation; replacement installs a new record
  // while existing mappings keep the old vnode alive through their shared
  // state.
  std::shared_ptr<GuestFileIoState> io_state;
  {
    const std::scoped_lock lock{global_identity_mutex};
    const auto identity = global_identities.find(normalized_path);
    if (identity != global_identities.end() &&
        identity->second.generation == generation &&
        identity->second.content_identity == *content_identity) {
      io_state = identity->second.io_state.lock();
      if (!io_state) {
        io_state = std::make_shared<GuestFileIoState>();
        io_state->file_descriptor = descriptor;
        descriptor = -1;
        identity->second.io_state = io_state;
      }
    }
  }
  if (!io_state) {
    io_state = std::make_shared<GuestFileIoState>();
    io_state->file_descriptor = descriptor;
    descriptor = -1;
  } else {
    close_descriptor();
  }

  auto mapping = std::make_shared<GuestFileBacking>(
      path, file_offset, file_offset + std::min<std::uintmax_t>(size, available));
  mapping->cache_path = normalized_path;
  mapping->file_size = file_size;
  mapping->modified = modified;
  mapping->generation = generation;
  mapping->content_identity = *content_identity;
  mapping->io_state = std::move(io_state);
  return mapping;
}

std::shared_ptr<GuestPageBacking> FilePageCache::load_page(
  const std::shared_ptr<GuestFileBacking> &mapping,
    std::uint64_t file_offset, std::uint32_t byte_count) {
  const Key key{mapping->cache_path, mapping->generation,
                mapping->content_identity, file_offset, byte_count};
  {
    const std::scoped_lock lock{mutex_};
    if (const auto cached = pages_.find(key); cached != pages_.end()) {
      touch_locked(cached);
      return cached->second.page;
    }
  }

  auto page = std::make_shared<GuestPageBacking>();
  page->file_backing_ = mapping;
  page->file_writeback_ = mapping;
  page->file_offset_ = file_offset;
  page->file_byte_count_ = byte_count;
  page->has_file_source_ = true;
  {
    const std::scoped_lock lock{mutex_};
    const auto [entry, inserted] = pages_.try_emplace(key, PageRecord{});
    if (!inserted) {
      touch_locked(entry);
      return entry->second.page;
    }
    entry->second.page = page;
    lru_.push_back(key);
    entry->second.lru_position = std::prev(lru_.end());
    evict_locked();
    return page;
  }
}

std::optional<std::vector<std::shared_ptr<GuestPageBacking>>>
FilePageCache::load_pages(const std::filesystem::path &path,
                          std::uint64_t file_offset, std::uint32_t size) {
  const auto mapping = open_mapping(path, file_offset, size);
  if (!mapping) return std::nullopt;

  std::vector<std::shared_ptr<GuestPageBacking>> result;
  result.reserve(static_cast<std::size_t>(
      (static_cast<std::uint64_t>(size) + guest_memory_page_size - 1U) /
      guest_memory_page_size));
  for (std::uint64_t offset = file_offset;
       offset < file_offset + size; offset += guest_memory_page_size) {
    const auto byte_count = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(guest_memory_page_size,
                                file_offset + size - offset));
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<std::streamoff>::max())) {
      return std::nullopt;
    }
    result.push_back(load_page(*mapping, offset, byte_count));
  }
  return result;
}

std::size_t FilePageCache::page_count() const {
  const std::scoped_lock lock{mutex_};
  return pages_.size();
}

FilePageCacheStats FilePageCache::stats() const {
  const std::scoped_lock lock{mutex_};
  return stats_;
}

} // namespace ilemu
