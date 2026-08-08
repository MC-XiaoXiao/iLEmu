#include "ilemu/file_page_cache.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <limits>
#include <tuple>

#include <fcntl.h>
#include <unistd.h>

namespace ilemu {
namespace {

constexpr std::uint64_t file_prefetch_bytes =
    guest_file_prefetch_pages * guest_memory_page_size;

std::atomic<std::uint64_t> global_shared_write_tracking_epoch{};
std::atomic<std::uint64_t> next_reservation_identity{1};

[[nodiscard]] std::string stable_path(const std::filesystem::path &path) {
  std::error_code error;
  const auto canonical = std::filesystem::canonical(path, error);
  return (error ? path.lexically_normal() : canonical).string();
}

} // namespace

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
    if (!io_state->stream) {
      io_state->stream = std::make_shared<std::ifstream>(
          file->path, std::ios::binary);
    }

    const auto aligned_start = file_offset_ & ~(file_prefetch_bytes - 1U);
    const auto read_start = std::max(file->first_offset, aligned_start);
    const auto read_size = std::min(file->end_offset - read_start,
                                    file_prefetch_bytes);
    const auto read_pages = static_cast<std::size_t>(
        (read_size + guest_memory_page_size - 1U) /
        guest_memory_page_size);
    if (io_state->stream && io_state->stream->is_open())
      io_state->stream->clear();
    if (io_state->stream && io_state->stream->is_open() &&
        read_start <= static_cast<std::uint64_t>(
                          std::numeric_limits<std::streamoff>::max())) {
      auto &stream = *io_state->stream;
      stream.seekg(static_cast<std::streamoff>(read_start));
      std::vector<std::byte> batch(static_cast<std::size_t>(read_size));
      stream.read(reinterpret_cast<char *>(batch.data()),
                  static_cast<std::streamsize>(batch.size()));
      const auto received = static_cast<std::size_t>(stream.gcount());
      if (!stream && !stream.eof()) {
        std::fill(batch.begin(), batch.end(), std::byte{});
      } else {
        batch.resize(received);
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
  const auto descriptor = ::open(file->path.c_str(), O_WRONLY | O_CLOEXEC);
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
  const auto close_result = ::close(descriptor);
  const auto complete = written == file_byte_count_ && close_result == 0;
  return complete;
}

bool FilePageCache::Key::operator<(const Key &other) const {
  return std::tie(path, content_identity, file_offset, byte_count) <
         std::tie(other.path, other.content_identity, other.file_offset,
                  other.byte_count);
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
                            std::uint32_t size) {
  if (size == 0 || file_offset % guest_memory_page_size != 0) {
    return std::nullopt;
  }

  std::error_code error;
  const auto file_size = std::filesystem::file_size(path, error);
  if (error || file_offset > file_size) {
    return std::nullopt;
  }
  const auto available = file_size - file_offset;
  const auto page_rounded_available =
      (available + guest_memory_page_size - 1U) &
      ~(static_cast<std::uintmax_t>(guest_memory_page_size) - 1U);
  if (size > page_rounded_available)
    return std::nullopt;
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error) return std::nullopt;
  const auto content_identity = sha256_file(path);
  if (!content_identity) return std::nullopt;
  const auto normalized_path = stable_path(path);

  {
    const std::scoped_lock lock{mutex_};
    const auto identity = identities_.find(normalized_path);
    if (identity == identities_.end()) {
      identities_.emplace(normalized_path,
                          Identity{file_size, modified, *content_identity});
    } else if (identity->second.content_identity != *content_identity) {
      erase_path_locked(normalized_path);
      identity->second =
          Identity{file_size, modified, *content_identity};
    } else {
      identity->second.file_size = file_size;
      identity->second.modified = modified;
    }
  }

  const auto final_size = std::filesystem::file_size(path, error);
  if (error || final_size != file_size) return std::nullopt;
  const auto final_modified = std::filesystem::last_write_time(path, error);
  if (error || final_modified != modified) return std::nullopt;

  auto mapping = std::make_shared<GuestFileBacking>(
      path, file_offset, file_offset + std::min<std::uintmax_t>(size, available));
  mapping->cache_path = normalized_path;
  mapping->file_size = file_size;
  mapping->modified = modified;
  mapping->content_identity = *content_identity;
  mapping->io_state->stream =
      std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!mapping->io_state->stream->is_open()) return std::nullopt;
  return mapping;
}

std::shared_ptr<GuestPageBacking> FilePageCache::load_page(
    const std::shared_ptr<GuestFileBacking> &mapping,
    std::uint64_t file_offset, std::uint32_t byte_count) {
  const Key key{mapping->cache_path, mapping->content_identity, file_offset,
                byte_count};
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

} // namespace ilemu
