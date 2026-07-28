#include "ilemu/address_space.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>

#include <sys/mman.h>

#include "ilemu/performance.hpp"

namespace ilemu {
namespace {

constexpr std::uint32_t page_base(std::uint32_t address) {
  return address & ~(AddressSpace::page_size - 1U);
}

constexpr std::uint8_t mapped_page_flag = 0x80U;

constexpr std::uint8_t permission_bits(MemoryPermission permissions) {
  return static_cast<std::uint8_t>(permissions);
}

bool range_overflows(std::uint32_t address, std::size_t size) {
  if (size == 0) {
    return false;
  }
  return size - 1 > std::numeric_limits<std::uint32_t>::max() - address;
}

std::uint64_t page_range_end(std::uint32_t address, std::size_t size) {
  return static_cast<std::uint64_t>(
             page_base(address + static_cast<std::uint32_t>(size - 1U))) +
         AddressSpace::page_size;
}

} // namespace

struct AddressSpace::JitPageTableStorage {
  static constexpr std::size_t byte_size =
      AddressSpace::page_count * sizeof(std::uint8_t *);

  JitPageTableStorage() {
    mapping = ::mmap(nullptr, byte_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
      mapping = nullptr;
      throw std::bad_alloc{};
    }
  }

  ~JitPageTableStorage() {
    if (mapping != nullptr) {
      static_cast<void>(::munmap(mapping, byte_size));
    }
  }

  [[nodiscard]] std::uint8_t **entries() const {
    return static_cast<std::uint8_t **>(mapping);
  }

  void clear() {
    if (::madvise(mapping, byte_size, MADV_DONTNEED) != 0) {
      std::memset(mapping, 0, byte_size);
    }
  }

  void *mapping{};
};

AddressSpace::AddressSpace()
    : observed_shared_write_tracking_epoch_{
          GuestPageBacking::shared_write_tracking_epoch()},
      file_page_cache_{std::make_shared<FilePageCache>()} {}

AddressSpace::~AddressSpace() = default;

void AddressSpace::set_parallel_access(bool enabled) {
  std::unique_lock lock{mutex_};
  parallel_access_ = enabled;
  jit_page_table_enabled_ = !enabled;
  if (!jit_read_page_table_ && !jit_write_page_table_) return;
  if (enabled) {
    clear_jit_page_table_locked();
  } else {
    for (const auto &[address, page] : *pages_) {
      static_cast<void>(page);
      refresh_jit_page_locked(address);
    }
  }
}

std::uint8_t **AddressSpace::jit_page_table() {
  return jit_write_page_table();
}

std::uint8_t **AddressSpace::jit_read_page_table() {
  auto lock = write_lock();
  if (!jit_page_table_enabled_) return nullptr;
  ensure_jit_page_tables_locked();
  return jit_read_page_table_->entries();
}

std::uint8_t **AddressSpace::jit_write_page_table() {
  auto lock = write_lock();
  if (!jit_page_table_enabled_) return nullptr;
  ensure_jit_page_tables_locked();
  return jit_write_page_table_->entries();
}

void AddressSpace::disable_jit_page_table() {
  auto lock = write_lock();
  jit_page_table_enabled_ = false;
  clear_jit_page_table_locked();
}

void AddressSpace::synchronize_shared_write_tracking() {
  auto target = GuestPageBacking::shared_write_tracking_epoch();
  if (target ==
      observed_shared_write_tracking_epoch_.load(std::memory_order_acquire)) {
    return;
  }
  auto lock = write_lock();
  // Capture the target again after serializing parallel address-space users.
  // A still newer epoch published during the scan remains pending for the
  // next safe point rather than being accidentally acknowledged here.
  target = GuestPageBacking::shared_write_tracking_epoch();
  if (target ==
      observed_shared_write_tracking_epoch_.load(std::memory_order_relaxed)) {
    return;
  }
  invalidate_shared_write_jit_pages_locked();
  observed_shared_write_tracking_epoch_.store(target,
                                              std::memory_order_release);
}

AddressSpace::ReadLock AddressSpace::read_lock() const {
  ReadLock lock{mutex_, std::defer_lock};
  if (parallel_access_) lock.lock();
  return lock;
}

AddressSpace::WriteLock AddressSpace::write_lock() {
  WriteLock lock{mutex_, std::defer_lock};
  if (parallel_access_) lock.lock();
  return lock;
}

bool AddressSpace::map(std::uint32_t address, std::uint32_t size,
                       MemoryPermission permissions) {
  if (size == 0 || range_overflows(address, size)) {
    return size == 0;
  }
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  auto lock = write_lock();
  invalidate_mapping_leases_locked(first, end);
  vm_map_.map_or(first, end, permissions);
  add_page_permissions_locked(first, end, permissions);
  refresh_jit_page_range_locked(first, end);
  return true;
}

bool AddressSpace::unmap(std::uint32_t address, std::uint32_t size) {
  if (size == 0 || range_overflows(address, size)) {
    return size == 0;
  }
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  auto lock = write_lock();
  invalidate_mapping_leases_locked(first, end);
  unmap_range_locked(first, end);
  return true;
}

void AddressSpace::unmap_range_locked(std::uint32_t address,
                                      std::uint64_t end) {
  vm_map_.unmap(address, end);
  unmap_file_mappings_locked(address, end);
  ensure_unique_page_map_locked();
  auto page = pages_->lower_bound(address);
  while (page != pages_->end() && page->first < end) {
    uncache_page_locked(page->first);
    page = pages_->erase(page);
  }
  clear_page_permissions_locked(address, end);
  refresh_jit_page_range_locked(address, end);
}

void AddressSpace::invalidate_mapping_leases_locked(
    std::uint32_t address, std::uint64_t end) {
  std::erase_if(mapping_leases_, [address, end](const auto &entry) {
    return static_cast<std::uint64_t>(entry.second.begin) < end &&
           entry.second.end > address;
  });
}

void AddressSpace::clear() {
  auto lock = write_lock();
  vm_map_.clear();
  pages_ = std::make_shared<PageMap>();
  file_mappings_.clear();
  for (auto &chunk : page_lookup_) chunk.reset();
  for (auto &chunk : page_permissions_) chunk.reset();
  clear_jit_page_table_locked();
  tracked_write_ranges_.clear();
  mapping_leases_.clear();
}

bool AddressSpace::protect(std::uint32_t address, std::uint32_t size,
                           MemoryPermission permissions) {
  if (size == 0 || range_overflows(address, size)) {
    return size == 0;
  }
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  auto lock = write_lock();
  if (!vm_map_.protect(first, end, permissions)) return false;
  set_page_permissions_locked(first, end, permissions);
  refresh_jit_page_range_locked(first, end);
  return true;
}

bool AddressSpace::copy_in(std::uint32_t address,
                           std::span<const std::byte> data) {
  if (range_overflows(address, data.size())) {
    return false;
  }
  if (data.empty()) return true;
  auto lock = write_lock();
  if (!range_accessible_locked(address, data.size(), MemoryPermission::None)) {
    return false;
  }
  std::size_t copied = 0;
  while (copied < data.size()) {
    const auto current = address + static_cast<std::uint32_t>(copied);
    auto &page = ensure_page_locked(current);
    const auto offset = current & (page_size - 1U);
    const auto chunk = std::min<std::size_t>(
        page_size - offset, data.size() - copied);
    auto &backing = writable_backing_locked(page);
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(copied), chunk,
                backing.bytes.begin() + offset);
    mark_shared_backing_written_locked(page);
    refresh_jit_page_locked(current);
    copied += chunk;
  }
  mark_written_locked(address, data.size());
  return true;
}

bool AddressSpace::copy_out(std::uint32_t address,
                            std::span<std::byte> data) const {
  if (range_overflows(address, data.size())) {
    return false;
  }
  if (data.empty())
    return true;
  for (;;) {
    {
      auto lock = read_lock();
      if (!range_accessible_locked(address, data.size(),
                                   MemoryPermission::Read)) {
        return false;
      }
      if (!range_needs_file_fault_locked(address, data.size())) {
        std::size_t copied = 0;
        while (copied < data.size()) {
          const auto current =
              address + static_cast<std::uint32_t>(copied);
          const auto *page = find_page_locked(current);
          const auto offset = current & (page_size - 1U);
          const auto chunk = std::min<std::size_t>(
              page_size - offset, data.size() - copied);
          if (page != nullptr && page->backing) {
            std::copy_n(page->backing->bytes.begin() + offset, chunk,
                        data.begin() +
                            static_cast<std::ptrdiff_t>(copied));
          } else {
            std::fill_n(
                data.begin() + static_cast<std::ptrdiff_t>(copied), chunk,
                std::byte{});
          }
          copied += chunk;
        }
        return true;
      }
    }
    if (!const_cast<AddressSpace *>(this)->fault_file_pages(address,
                                                            data.size())) {
      return false;
    }
  }
}

bool AddressSpace::map_file(std::uint32_t address, std::uint32_t size,
                            MemoryPermission permissions,
                            const std::filesystem::path &path,
                            std::uint64_t file_offset) {
  if (size == 0 || range_overflows(address, size) ||
      address % page_size != 0 || file_offset % page_size != 0) {
    return false;
  }
  const auto backing = file_page_cache_->open_mapping(path, file_offset, size);
  if (!backing) return false;

  auto lock = write_lock();
  const auto end = page_range_end(address, size);
  if (vm_map_.overlaps(address, end)) return false;
  invalidate_mapping_leases_locked(address, end);
  const auto [mapping, inserted] = file_mappings_.emplace(
      address, FileMapping{end, file_offset, *backing});
  static_cast<void>(mapping);
  if (!inserted) return false;
  vm_map_.map_or(address, end, permissions);
  add_page_permissions_locked(address, end, permissions);
  return true;
}

std::optional<std::vector<std::shared_ptr<GuestPageBacking>>>
AddressSpace::share_pages(std::uint32_t address, std::uint32_t size) {
  if (size == 0 || address % page_size != 0 || size % page_size != 0 ||
      range_overflows(address, size)) {
    return std::nullopt;
  }

  auto lock = write_lock();
  const auto end = page_range_end(address, size);
  if (!range_accessible_locked(address, size, MemoryPermission::None))
    return std::nullopt;

  std::vector<std::shared_ptr<GuestPageBacking>> result;
  result.reserve(size / page_size);
  bool has_tracked_shared_backing = false;
  for (std::uint64_t base = address; base < end; base += page_size) {
    auto &page = ensure_page_locked(static_cast<std::uint32_t>(base));
    if (!page.backing) {
      page.backing = std::make_shared<GuestPageBacking>();
    } else if (page.file_cached ||
               (page.copy_on_write_possible && !page.shared_writable &&
                !page.backing.unique())) {
      page.backing = std::make_shared<GuestPageBacking>(*page.backing);
    }
    page.file_cached = false;
    page.shared_writable = true;
    page.copy_on_write_possible = false;
    if (tracks_write_locked(static_cast<std::uint32_t>(base), page_size)) {
      static_cast<void>(page.backing->enable_shared_write_tracking());
      has_tracked_shared_backing = true;
    }
    refresh_jit_page_locked(static_cast<std::uint32_t>(base));
    result.push_back(page.backing);
  }
  if (has_tracked_shared_backing)
    invalidate_shared_write_jit_pages_locked();
  return result;
}

bool AddressSpace::map_page_backings(
    std::uint32_t address, std::uint32_t size,
    MemoryPermission permissions,
    std::span<const std::shared_ptr<GuestPageBacking>> backings,
    PageMappingMode mode, std::uint64_t *mapping_lease_token) {
  if (mapping_lease_token)
    *mapping_lease_token = 0;
  if (size == 0 || address % page_size != 0 || size % page_size != 0 ||
      range_overflows(address, size) ||
      backings.size() != size / page_size ||
      std::any_of(backings.begin(), backings.end(),
                  [](const auto &backing) { return !backing; })) {
    return false;
  }

  auto lock = write_lock();
  const auto end = page_range_end(address, size);
  if (vm_map_.overlaps(address, end)) return false;
  invalidate_mapping_leases_locked(address, end);
  const auto shared_writable = mode == PageMappingMode::Shared;
  ensure_unique_page_map_locked();
  for (std::size_t index = 0; index < backings.size(); ++index) {
    const auto base =
        address + static_cast<std::uint32_t>(index * page_size);
    if (pages_->contains(base))
      return false;
  }
  bool has_tracked_shared_backing = false;
  for (std::size_t index = 0; index < backings.size(); ++index) {
    const auto base =
        address + static_cast<std::uint32_t>(index * page_size);
    auto [page, inserted] = pages_->emplace(
        base, Page{backings[index], 0, false, shared_writable,
                   !shared_writable});
    static_cast<void>(inserted);
    if (shared_writable && tracks_write_locked(base, page_size)) {
      static_cast<void>(page->second.backing->enable_shared_write_tracking());
      has_tracked_shared_backing = true;
    }
    cache_page_locked(base, page->second);
  }
  vm_map_.map_or(address, end, permissions);
  add_page_permissions_locked(address, end, permissions);
  if (has_tracked_shared_backing)
    invalidate_shared_write_jit_pages_locked();
  refresh_jit_page_range_locked(address, end);
  if (mapping_lease_token) {
    auto token = next_mapping_lease_token_++;
    if (token == 0U)
      token = next_mapping_lease_token_++;
    while (mapping_leases_.contains(token)) {
      token = next_mapping_lease_token_++;
      if (token == 0U)
        token = next_mapping_lease_token_++;
    }
    mapping_leases_.emplace(token, MappingLease{address, end});
    *mapping_lease_token = token;
  }
  return true;
}

bool AddressSpace::unmap_mapping_lease(
    std::uint64_t mapping_lease_token) {
  if (mapping_lease_token == 0U)
    return false;
  auto lock = write_lock();
  const auto lease = mapping_leases_.find(mapping_lease_token);
  if (lease == mapping_leases_.end())
    return false;
  const auto range = lease->second;
  mapping_leases_.erase(lease);
  invalidate_mapping_leases_locked(range.begin, range.end);
  unmap_range_locked(range.begin, range.end);
  return true;
}

std::optional<std::vector<std::byte>>
AddressSpace::read_bytes(std::uint32_t address, std::size_t size) const {
  for (;;) {
    {
      auto lock = read_lock();
      if (!range_accessible_locked(address, size, MemoryPermission::Read)) {
        return std::nullopt;
      }
      if (!range_needs_file_fault_locked(address, size)) {
        std::vector<std::byte> result(size);
        std::size_t copied = 0;
        while (copied < size) {
          const auto current = address + static_cast<std::uint32_t>(copied);
          const auto *page = find_page_locked(current);
          const auto offset = current & (page_size - 1U);
          const auto chunk =
              std::min<std::size_t>(page_size - offset, size - copied);
          if (page != nullptr && page->backing) {
            std::copy_n(page->backing->bytes.begin() + offset, chunk,
                        result.begin() + static_cast<std::ptrdiff_t>(copied));
          } else {
            std::fill_n(
                result.begin() + static_cast<std::ptrdiff_t>(copied), chunk,
                std::byte{});
          }
          copied += chunk;
        }
        return result;
      }
    }
    if (!const_cast<AddressSpace *>(this)->fault_file_pages(address, size)) {
      return std::nullopt;
    }
  }
}

std::optional<std::string>
AddressSpace::read_c_string(std::uint32_t address,
                            std::size_t maximum_size) const {
  std::string result;
  result.reserve(std::min<std::size_t>(maximum_size, 256));
  std::size_t consumed = 0;
  while (consumed < maximum_size) {
    if (range_overflows(address, consumed + 1U)) return std::nullopt;
    const auto current = address + static_cast<std::uint32_t>(consumed);
    bool needs_fault = false;
    {
      auto lock = read_lock();
      if (!range_accessible_locked(current, 1, MemoryPermission::Read)) {
        return std::nullopt;
      }
      const auto *page = find_page_locked(current);
      if ((page == nullptr || !page->backing) &&
          find_file_mapping_locked(current) != nullptr) {
        needs_fault = true;
      } else {
        const auto offset = current & (page_size - 1U);
        const auto chunk = std::min<std::size_t>(
            page_size - offset, maximum_size - consumed);
        if (page == nullptr || !page->backing) return result;
        for (std::size_t index = 0; index < chunk; ++index) {
          const auto value =
              std::to_integer<char>(page->backing->bytes[offset + index]);
          if (value == '\0') return result;
          result.push_back(value);
        }
        consumed += chunk;
      }
    }
    if (needs_fault &&
        !const_cast<AddressSpace *>(this)->fault_file_pages(current, 1)) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

const AddressSpace::Page *
AddressSpace::find_page_locked(std::uint32_t address) const {
  const auto index = static_cast<std::size_t>(address / page_size);
  const auto &chunk = page_lookup_[index / page_lookup_chunk_size];
  return chunk ? (*chunk)[index % page_lookup_chunk_size] : nullptr;
}

AddressSpace::Page *AddressSpace::find_page_locked(std::uint32_t address) {
  const auto index = static_cast<std::size_t>(address / page_size);
  const auto &chunk = page_lookup_[index / page_lookup_chunk_size];
  return chunk ? (*chunk)[index % page_lookup_chunk_size] : nullptr;
}

const AddressSpace::FileMapping *
AddressSpace::find_file_mapping_locked(std::uint32_t address) const {
  const auto after = file_mappings_.upper_bound(address);
  if (after == file_mappings_.begin()) return nullptr;
  const auto mapping = std::prev(after);
  return address < mapping->second.end ? &mapping->second : nullptr;
}

AddressSpace::Page &AddressSpace::ensure_page_locked(std::uint32_t address) {
  ensure_unique_page_map_locked();
  const auto base = page_base(address);
  auto [page, inserted] = pages_->try_emplace(base);
  if (!page->second.backing) {
    if (const auto *mapping = find_file_mapping_locked(base)) {
      performance_counters().record_page_miss();
      const auto mapping_start = static_cast<std::uint64_t>(
          std::prev(file_mappings_.upper_bound(base))->first);
      const auto file_offset =
          mapping->file_offset + (static_cast<std::uint64_t>(base) -
                                  mapping_start);
      const auto byte_count = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(page_size,
                                  mapping->backing->end_offset - file_offset));
      auto backing = file_page_cache_->load_page(
          mapping->backing, file_offset, byte_count);
      // A resident AddressSpace page always owns fully initialized bytes.
      // Page-in remains lazy at the range level, while ordinary guest reads
      // no longer re-enter GuestPageBacking's one-time materialization lock.
      backing->materialize();
      page->second.backing = std::move(backing);
      page->second.file_cached = true;
      page->second.copy_on_write_possible = true;
    }
  }
  if (inserted) cache_page_locked(base, page->second);
  return page->second;
}

bool AddressSpace::range_needs_file_fault_locked(std::uint32_t address,
                                                 std::size_t size) const {
  if (size == 0 || range_overflows(address, size)) return false;
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  for (std::uint64_t base = first; base < end; base += page_size) {
    const auto current = static_cast<std::uint32_t>(base);
    const auto *page = find_page_locked(current);
    if ((page == nullptr || !page->backing) &&
        find_file_mapping_locked(current) != nullptr) {
      return true;
    }
  }
  return false;
}

bool AddressSpace::fault_file_pages(std::uint32_t address,
                                    std::size_t size) {
  if (size == 0 || range_overflows(address, size)) return size == 0;
  auto lock = write_lock();
  if (!range_accessible_locked(address, size, MemoryPermission::None)) {
    return false;
  }
  ensure_unique_page_map_locked();
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  for (std::uint64_t base = first; base < end; base += page_size) {
    const auto current = static_cast<std::uint32_t>(base);
    const auto *page = find_page_locked(current);
    if (page != nullptr && page->backing) continue;

    const auto mapping_after = file_mappings_.upper_bound(current);
    if (mapping_after == file_mappings_.begin()) continue;
    const auto mapping_entry = std::prev(mapping_after);
    const auto mapping_start = mapping_entry->first;
    const auto &mapping = mapping_entry->second;
    if (current >= mapping.end) continue;
    performance_counters().record_page_miss();

    constexpr std::uint64_t cluster_bytes =
        guest_file_prefetch_pages * page_size;
    const auto current_file_offset =
        mapping.file_offset +
        (static_cast<std::uint64_t>(current) - mapping_start);
    const auto cluster_file_start = std::max<std::uint64_t>(
        mapping.backing->first_offset,
        current_file_offset & ~(cluster_bytes - 1U));
    const auto cluster_file_end = std::min<std::uint64_t>(
        mapping.backing->end_offset, cluster_file_start + cluster_bytes);
    const auto cluster_guest_start =
        static_cast<std::uint64_t>(mapping_start) +
        (cluster_file_start - mapping.file_offset);

    for (std::uint64_t file_page = cluster_file_start,
                       guest_page = cluster_guest_start;
         file_page < cluster_file_end && guest_page < mapping.end;
         file_page += page_size, guest_page += page_size) {
      const auto guest_base = static_cast<std::uint32_t>(guest_page);
      auto [resident, inserted] = pages_->try_emplace(guest_base);
      if (!resident->second.backing) {
        const auto byte_count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(page_size,
                                    cluster_file_end - file_page));
        auto backing = file_page_cache_->load_page(
            mapping.backing, file_page, byte_count);
        // The vnode-style cluster fault publishes ready pages as one unit.
        // GuestFileBacking performs only one host read for the cluster; the
        // remaining calls consume its prefetched pages.
        backing->materialize();
        resident->second.backing = std::move(backing);
        resident->second.file_cached = true;
        resident->second.copy_on_write_possible = true;
      }
      if (inserted) cache_page_locked(guest_base, resident->second);
      refresh_jit_page_locked(guest_base);
    }
  }
  return true;
}

void AddressSpace::unmap_file_mappings_locked(std::uint32_t address,
                                              std::uint64_t end) {
  if (file_mappings_.empty()) return;
  auto mapping = file_mappings_.lower_bound(address);
  if (mapping != file_mappings_.begin()) {
    const auto previous = std::prev(mapping);
    if (previous->second.end > address) mapping = previous;
  }

  std::vector<std::pair<std::uint32_t, FileMapping>> replacements;
  const auto make_backing = [](const GuestFileBacking &source,
                               std::uint64_t first_offset,
                               std::uint64_t end_offset) {
    auto backing = std::make_shared<GuestFileBacking>(
        source.path, first_offset, end_offset);
    backing->cache_path = source.cache_path;
    backing->file_size = source.file_size;
    backing->modified = source.modified;
    return backing;
  };

  while (mapping != file_mappings_.end() && mapping->first < end) {
    const auto start = mapping->first;
    const auto source = mapping->second;
    if (source.end <= address) {
      ++mapping;
      continue;
    }
    mapping = file_mappings_.erase(mapping);

    if (start < address) {
      const auto left_file_end = std::min<std::uint64_t>(
          source.backing->end_offset,
          source.file_offset +
              (static_cast<std::uint64_t>(address) - start));
      replacements.emplace_back(
          start, FileMapping{address, source.file_offset,
                             make_backing(*source.backing, source.file_offset,
                                          left_file_end)});
    }
    if (source.end > end) {
      const auto right_start = static_cast<std::uint32_t>(end);
      const auto right_file_offset =
          source.file_offset + (end - static_cast<std::uint64_t>(start));
      replacements.emplace_back(
          right_start,
          FileMapping{source.end, right_file_offset,
                      make_backing(*source.backing, right_file_offset,
                                   source.backing->end_offset)});
    }
  }
  for (auto &replacement : replacements) {
    file_mappings_.emplace(replacement.first,
                           std::move(replacement.second));
  }
}

void AddressSpace::cache_page_locked(std::uint32_t address, Page &page) {
  const auto index = static_cast<std::size_t>(address / page_size);
  auto &chunk = page_lookup_[index / page_lookup_chunk_size];
  if (!chunk) chunk = std::make_unique<PageLookupChunk>();
  (*chunk)[index % page_lookup_chunk_size] = &page;
}

void AddressSpace::uncache_page_locked(std::uint32_t address) {
  const auto index = static_cast<std::size_t>(address / page_size);
  auto &chunk = page_lookup_[index / page_lookup_chunk_size];
  if (chunk) (*chunk)[index % page_lookup_chunk_size] = nullptr;
}

void AddressSpace::ensure_unique_page_map_locked() {
  if (pages_.unique()) return;
  pages_ = std::make_shared<PageMap>(*pages_);
  rebuild_page_lookup_locked();
}

void AddressSpace::rebuild_page_lookup_locked() {
  for (auto &chunk : page_lookup_) chunk.reset();
  for (auto &[address, page] : *pages_) cache_page_locked(address, page);
}

void AddressSpace::ensure_jit_page_tables_locked() {
  if (jit_read_page_table_ && jit_write_page_table_)
    return;
  if (!jit_read_page_table_)
    jit_read_page_table_ = std::make_unique<JitPageTableStorage>();
  if (!jit_write_page_table_)
    jit_write_page_table_ = std::make_unique<JitPageTableStorage>();
  for (const auto &[address, page] : *pages_) {
    static_cast<void>(page);
    refresh_jit_page_locked(address);
  }
}

void AddressSpace::refresh_jit_page_locked(std::uint32_t address) {
  if (!jit_read_page_table_ && !jit_write_page_table_) return;
  const auto base = page_base(address);
  auto *read_entry = jit_read_page_table_
                         ? &jit_read_page_table_->entries()[base / page_size]
                         : nullptr;
  auto *write_entry = jit_write_page_table_
                          ? &jit_write_page_table_->entries()[base / page_size]
                          : nullptr;
  if (read_entry) *read_entry = nullptr;
  if (write_entry) *write_entry = nullptr;
  if (!jit_page_table_enabled_) return;

  const auto flags = page_permission_locked(base / page_size);
  constexpr auto read_required = static_cast<std::uint8_t>(
      mapped_page_flag | permission_bits(MemoryPermission::Read));
  const auto *page = find_page_locked(base);
  if (read_entry && (flags & read_required) == read_required &&
      page != nullptr && page->backing) {
    *read_entry =
        reinterpret_cast<std::uint8_t *>(page->backing->bytes.data());
  }

  constexpr auto write_required = static_cast<std::uint8_t>(
      mapped_page_flag | permission_bits(MemoryPermission::Read) |
      permission_bits(MemoryPermission::Write));
  if (!write_entry || (flags & write_required) != write_required ||
      tracks_write_locked(base, page_size) ||
      (page != nullptr && page->backing &&
       page->backing->shared_write_tracking_enabled())) {
    return;
  }
  if (page == nullptr || !page->backing || page->file_cached ||
      (page->copy_on_write_possible && !page->shared_writable)) {
    return;
  }
  *write_entry =
      reinterpret_cast<std::uint8_t *>(page->backing->bytes.data());
}

void AddressSpace::refresh_jit_page_range_locked(std::uint32_t address,
                                                 std::uint64_t end) {
  if ((!jit_read_page_table_ && !jit_write_page_table_) || end <= address) {
    return;
  }
  const auto first = page_base(address);
  for (std::uint64_t base = first; base < end; base += page_size) {
    refresh_jit_page_locked(static_cast<std::uint32_t>(base));
  }
}

void AddressSpace::invalidate_shared_write_jit_pages_locked() {
  if (!jit_write_page_table_)
    return;
  auto **entries = jit_write_page_table_->entries();
  for (const auto &[address, page] : *pages_) {
    if (page.backing && page.backing->shared_write_tracking_enabled())
      entries[address / page_size] = nullptr;
  }
}

void AddressSpace::clear_jit_page_table_locked() {
  if (jit_read_page_table_) jit_read_page_table_->clear();
  if (jit_write_page_table_) jit_write_page_table_->clear();
}

std::byte AddressSpace::read_byte_locked(const Page *page,
                                         std::uint32_t offset) {
  if (page == nullptr || !page->backing) return std::byte{};
  return page->backing->bytes[offset];
}

GuestPageBacking &
AddressSpace::writable_backing_locked(Page &page) {
  if (!page.backing) {
    page.backing = std::make_shared<GuestPageBacking>();
  } else {
    if (page.file_cached ||
        (page.copy_on_write_possible && !page.shared_writable &&
         !page.backing.unique())) {
      page.backing = std::make_shared<GuestPageBacking>(*page.backing);
    }
  }
  page.file_cached = false;
  page.copy_on_write_possible = false;
  return *page.backing;
}

void AddressSpace::mark_shared_backing_written_locked(Page &page) {
  if (page.backing)
    page.backing->mark_shared_write();
}

bool AddressSpace::tracks_write_locked(std::uint32_t address,
                                       std::size_t size) const {
  if (tracked_write_ranges_.empty() || size == 0) return false;
  const auto begin = static_cast<std::uint64_t>(address);
  const auto end = begin + size;
  return std::ranges::any_of(
      tracked_write_ranges_, [begin, end](const TrackedWriteRange &range) {
        return begin < range.end && range.begin < end;
      });
}

void AddressSpace::mark_written_locked(std::uint32_t address,
                                       std::size_t size) {
  if (!tracks_write_locked(address, size)) return;
  ++write_generation_;
  const auto first = page_base(address);
  const auto last = page_base(
      address + static_cast<std::uint32_t>(size - 1U));
  for (std::uint64_t base = first; base <= last; base += page_size) {
    auto *page = find_page_locked(static_cast<std::uint32_t>(base));
    if (page != nullptr) page->write_generation = write_generation_;
  }
}

bool AddressSpace::range_accessible_locked(std::uint32_t address,
                                           std::size_t size,
                                           MemoryPermission access) const {
  if (range_overflows(address, size)) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  const auto required = permission_bits(access);
  for (std::uint64_t base = first; base < end; base += page_size) {
    const auto flags = page_permission_locked(base / page_size);
    if ((flags & mapped_page_flag) == 0U || (flags & required) != required)
      return false;
  }
  return true;
}

template <typename T>
std::optional<T> AddressSpace::read_integer(std::uint32_t address,
                                            MemoryPermission access) const {
  static_assert(std::is_unsigned_v<T>);
  for (;;) {
    {
      auto lock = read_lock();
      if (!range_accessible_locked(address, sizeof(T), access)) {
        return std::nullopt;
      }
      const auto offset = address & (page_size - 1U);
      if (offset <= page_size - sizeof(T)) {
        const auto *page = find_page_locked(address);
        if (page != nullptr && page->backing) {
          T value = 0;
          for (std::size_t index = 0; index < sizeof(T); ++index) {
            value |= static_cast<T>(
                std::to_integer<T>(page->backing->bytes[offset + index])
                << (index * 8U));
          }
          return value;
        }
        if (find_file_mapping_locked(address) == nullptr) return T{};
      } else if (!range_needs_file_fault_locked(address, sizeof(T))) {
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
          const auto current = address + static_cast<std::uint32_t>(i);
          const auto *page = find_page_locked(current);
          const auto byte = std::to_integer<T>(
              read_byte_locked(page, current & (page_size - 1U)));
          value |= static_cast<T>(byte << (i * 8U));
        }
        return value;
      }
    }
    if (!const_cast<AddressSpace *>(this)->fault_file_pages(address,
                                                            sizeof(T))) {
      return std::nullopt;
    }
  }
}

template <typename T>
bool AddressSpace::write_integer(std::uint32_t address, T value) {
  static_assert(std::is_unsigned_v<T>);
  auto lock = write_lock();
  if (!range_accessible_locked(address, sizeof(T), MemoryPermission::Write)) {
    return false;
  }
  ensure_unique_page_map_locked();
  const auto offset = address & (page_size - 1U);
  if (offset <= page_size - sizeof(T)) {
    auto *resident = find_page_locked(address);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(address);
    auto &backing = writable_backing_locked(page);
    refresh_jit_page_locked(address);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      backing.bytes[offset + index] = static_cast<std::byte>(
          (value >> (index * 8U)) & static_cast<T>(0xffU));
    }
    mark_shared_backing_written_locked(page);
    if (tracks_write_locked(address, sizeof(T))) {
      page.write_generation = ++write_generation_;
    }
    return true;
  }
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto current = address + static_cast<std::uint32_t>(i);
    auto *resident = find_page_locked(current);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(current);
    auto &backing = writable_backing_locked(page);
    refresh_jit_page_locked(current);
    backing.bytes[current & (page_size - 1U)] =
        static_cast<std::byte>((value >> (i * 8U)) & static_cast<T>(0xffU));
    mark_shared_backing_written_locked(page);
  }
  mark_written_locked(address, sizeof(T));
  return true;
}

template <typename T>
bool AddressSpace::compare_exchange_integer(std::uint32_t address, T expected,
                                            T value) {
  static_assert(std::is_unsigned_v<T>);
  auto lock = write_lock();
  if (!range_accessible_locked(address, sizeof(T),
                               MemoryPermission::Read |
                                   MemoryPermission::Write)) {
    return false;
  }
  ensure_unique_page_map_locked();
  const auto offset = address & (page_size - 1U);
  if (offset <= page_size - sizeof(T)) {
    auto *resident = find_page_locked(address);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(address);
    T current_value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      current_value |= static_cast<T>(
          std::to_integer<T>(read_byte_locked(
              &page, offset + static_cast<std::uint32_t>(index)))
          << (index * 8U));
    }
    if (current_value != expected) return false;
    auto &backing = writable_backing_locked(page);
    refresh_jit_page_locked(address);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      backing.bytes[offset + index] = static_cast<std::byte>(
          (value >> (index * 8U)) & static_cast<T>(0xffU));
    }
    mark_shared_backing_written_locked(page);
    if (tracks_write_locked(address, sizeof(T))) {
      page.write_generation = ++write_generation_;
    }
    return true;
  }
  T current_value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto current = address + static_cast<std::uint32_t>(i);
    auto *resident = find_page_locked(current);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(current);
    current_value |= static_cast<T>(
        std::to_integer<T>(
            read_byte_locked(&page, current & (page_size - 1U)))
        << (i * 8U));
  }
  if (current_value != expected) {
    return false;
  }
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const auto current = address + static_cast<std::uint32_t>(i);
    auto *resident = find_page_locked(current);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(current);
    auto &backing = writable_backing_locked(page);
    refresh_jit_page_locked(current);
    backing.bytes[current & (page_size - 1U)] =
        static_cast<std::byte>((value >> (i * 8U)) & static_cast<T>(0xffU));
    mark_shared_backing_written_locked(page);
  }
  mark_written_locked(address, sizeof(T));
  return true;
}

template <typename T>
std::optional<T> AddressSpace::exchange_integer(std::uint32_t address,
                                                T value) {
  static_assert(std::is_unsigned_v<T>);
  auto lock = write_lock();
  if (!range_accessible_locked(address, sizeof(T),
                               MemoryPermission::Read |
                                   MemoryPermission::Write)) {
    return std::nullopt;
  }
  ensure_unique_page_map_locked();
  const auto offset = address & (page_size - 1U);
  if (offset <= page_size - sizeof(T)) {
    auto *resident = find_page_locked(address);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(address);
    T previous = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      previous |= static_cast<T>(
          std::to_integer<T>(read_byte_locked(
              &page, offset + static_cast<std::uint32_t>(index)))
          << (index * 8U));
    }
    auto &backing = writable_backing_locked(page);
    refresh_jit_page_locked(address);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
      backing.bytes[offset + index] = static_cast<std::byte>(
          (value >> (index * 8U)) & static_cast<T>(0xffU));
    }
    mark_shared_backing_written_locked(page);
    if (tracks_write_locked(address, sizeof(T))) {
      page.write_generation = ++write_generation_;
    }
    return previous;
  }
  T previous = 0;
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    const auto current = address + static_cast<std::uint32_t>(index);
    auto *resident = find_page_locked(current);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(current);
    previous |= static_cast<T>(
        std::to_integer<T>(
            read_byte_locked(&page, current & (page_size - 1U)))
        << (index * 8U));
  }
  for (std::size_t index = 0; index < sizeof(T); ++index) {
    const auto current = address + static_cast<std::uint32_t>(index);
    auto *resident = find_page_locked(current);
    auto &page = resident != nullptr && resident->backing
                     ? *resident
                     : ensure_page_locked(current);
    auto &backing = writable_backing_locked(page);
    refresh_jit_page_locked(current);
    backing.bytes[current & (page_size - 1U)] =
        static_cast<std::byte>((value >> (index * 8U)) &
                               static_cast<T>(0xffU));
    mark_shared_backing_written_locked(page);
  }
  mark_written_locked(address, sizeof(T));
  return previous;
}

std::optional<std::uint8_t> AddressSpace::read8(std::uint32_t address,
                                                MemoryPermission access) const {
  return read_integer<std::uint8_t>(address, access);
}
std::optional<std::uint16_t>
AddressSpace::read16(std::uint32_t address, MemoryPermission access) const {
  return read_integer<std::uint16_t>(address, access);
}
std::optional<std::uint32_t>
AddressSpace::read32(std::uint32_t address, MemoryPermission access) const {
  return read_integer<std::uint32_t>(address, access);
}
std::optional<std::uint64_t>
AddressSpace::read64(std::uint32_t address, MemoryPermission access) const {
  return read_integer<std::uint64_t>(address, access);
}

bool AddressSpace::write8(std::uint32_t address, std::uint8_t value) {
  return write_integer(address, value);
}
bool AddressSpace::write16(std::uint32_t address, std::uint16_t value) {
  return write_integer(address, value);
}
bool AddressSpace::write32(std::uint32_t address, std::uint32_t value) {
  return write_integer(address, value);
}
bool AddressSpace::write64(std::uint32_t address, std::uint64_t value) {
  return write_integer(address, value);
}

bool AddressSpace::accessible(std::uint32_t address, std::size_t size,
                              MemoryPermission access) const {
  auto lock = read_lock();
  return range_accessible_locked(address, size, access);
}

bool AddressSpace::compare_exchange8(std::uint32_t address,
                                     std::uint8_t expected,
                                     std::uint8_t value) {
  return compare_exchange_integer(address, expected, value);
}
bool AddressSpace::compare_exchange16(std::uint32_t address,
                                      std::uint16_t expected,
                                      std::uint16_t value) {
  return compare_exchange_integer(address, expected, value);
}
bool AddressSpace::compare_exchange32(std::uint32_t address,
                                      std::uint32_t expected,
                                      std::uint32_t value) {
  return compare_exchange_integer(address, expected, value);
}
bool AddressSpace::compare_exchange64(std::uint32_t address,
                                      std::uint64_t expected,
                                      std::uint64_t value) {
  return compare_exchange_integer(address, expected, value);
}

std::optional<std::uint8_t> AddressSpace::exchange8(std::uint32_t address,
                                                    std::uint8_t value) {
  return exchange_integer(address, value);
}

std::optional<std::uint32_t> AddressSpace::exchange32(std::uint32_t address,
                                                      std::uint32_t value) {
  return exchange_integer(address, value);
}

bool AddressSpace::mapped(std::uint32_t address, std::size_t size) const {
  auto lock = read_lock();
  return range_accessible_locked(address, size, MemoryPermission::None);
}

bool AddressSpace::track_write_generation(std::uint32_t address,
                                          std::size_t size) {
  if (size == 0 || range_overflows(address, size)) return false;
  auto lock = write_lock();
  if (!range_accessible_locked(address, size, MemoryPermission::None)) {
    return false;
  }
  TrackedWriteRange tracked{
      address, static_cast<std::uint64_t>(address) + size};
  for (std::size_t index = 0; index < tracked_write_ranges_.size();) {
    const auto &existing = tracked_write_ranges_[index];
    if (tracked.end < existing.begin || existing.end < tracked.begin) {
      ++index;
      continue;
    }
    tracked.begin = std::min(tracked.begin, existing.begin);
    tracked.end = std::max(tracked.end, existing.end);
    tracked_write_ranges_.erase(
        tracked_write_ranges_.begin() +
        static_cast<std::ptrdiff_t>(index));
    index = 0;
  }
  tracked_write_ranges_.push_back(tracked);
  std::ranges::sort(
      tracked_write_ranges_, {},
      [](const TrackedWriteRange &range) { return range.begin; });
  bool has_shared_backing = false;
  const auto tracking_end = page_range_end(address, size);
  for (std::uint64_t base = page_base(address); base < tracking_end;
       base += page_size) {
    auto *page = find_page_locked(static_cast<std::uint32_t>(base));
    if (page == nullptr || !page->shared_writable || !page->backing)
      continue;
    static_cast<void>(page->backing->enable_shared_write_tracking());
    has_shared_backing = true;
  }
  if (has_shared_backing)
    invalidate_shared_write_jit_pages_locked();
  refresh_jit_page_range_locked(page_base(address), tracking_end);
  return true;
}

std::optional<std::uint64_t> AddressSpace::range_write_generation(
    std::uint32_t address, std::size_t size) const {
  if (size == 0 || range_overflows(address, size)) return std::nullopt;
  const auto first = page_base(address);
  const auto last = page_base(
      address + static_cast<std::uint32_t>(size - 1U));
  auto lock = read_lock();
  if (!range_accessible_locked(address, size, MemoryPermission::None)) {
    return std::nullopt;
  }
  std::uint64_t generation = 0;
  for (std::uint64_t base = first; base <= last; base += page_size) {
    const auto *page = find_page_locked(static_cast<std::uint32_t>(base));
    if (page != nullptr)
      generation = std::max(generation, page->write_generation);
  }
  return generation;
}

std::optional<AddressSpace::WriteGenerationChanges>
AddressSpace::write_generation_changes(
    std::uint32_t address, std::size_t size,
    std::uint64_t after_generation) const {
  if (size == 0 || range_overflows(address, size))
    return std::nullopt;
  const auto requested_begin = static_cast<std::uint64_t>(address);
  const auto requested_end = requested_begin + size;
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  auto lock = read_lock();
  if (!range_accessible_locked(address, size, MemoryPermission::None))
    return std::nullopt;

  WriteGenerationChanges result;
  for (std::uint64_t base = first; base < end; base += page_size) {
    const auto *page =
        find_page_locked(static_cast<std::uint32_t>(base));
    const auto generation = page ? page->write_generation : 0U;
    result.generation = std::max(result.generation, generation);
    if (generation <= after_generation)
      continue;
    const auto dirty_begin = std::max(base, requested_begin);
    const auto dirty_end = std::min(base + page_size, requested_end);
    if (dirty_end <= dirty_begin)
      continue;
    if (!result.ranges.empty()) {
      auto &previous = result.ranges.back();
      const auto previous_end =
          static_cast<std::uint64_t>(previous.address) + previous.size;
      if (previous_end == dirty_begin) {
        previous.size += static_cast<std::uint32_t>(dirty_end - dirty_begin);
        continue;
      }
    }
    result.ranges.push_back(
        WrittenRange{static_cast<std::uint32_t>(dirty_begin),
                     static_cast<std::uint32_t>(dirty_end - dirty_begin)});
  }
  return result;
}

std::optional<AddressSpace::SharedWriteGenerationChanges>
AddressSpace::shared_write_generation_changes(
    std::uint32_t address, std::size_t size,
    std::span<const std::uint64_t> after_page_generations) const {
  if (size == 0 || range_overflows(address, size))
    return std::nullopt;
  const auto requested_begin = static_cast<std::uint64_t>(address);
  const auto requested_end = requested_begin + size;
  const auto first = page_base(address);
  const auto end = page_range_end(address, size);
  auto lock = read_lock();
  if (!range_accessible_locked(address, size, MemoryPermission::None))
    return std::nullopt;

  SharedWriteGenerationChanges result;
  const auto range_page_count =
      static_cast<std::size_t>((end - first) / page_size);
  result.page_generations.reserve(range_page_count);
  const auto has_baseline = after_page_generations.size() == range_page_count;
  std::size_t page_index = 0;
  for (std::uint64_t base = first; base < end; base += page_size) {
    const auto *page = find_page_locked(static_cast<std::uint32_t>(base));
    const auto generation =
        page && page->backing ? page->backing->shared_write_generation() : 0U;
    result.page_generations.push_back(generation);
    const auto changed =
        !has_baseline || generation != after_page_generations[page_index];
    ++page_index;
    if (!changed)
      continue;
    const auto dirty_begin = std::max(base, requested_begin);
    const auto dirty_end =
        std::min(base + page_size, requested_end);
    if (dirty_end <= dirty_begin)
      continue;
    if (!result.ranges.empty()) {
      auto &previous = result.ranges.back();
      const auto previous_end =
          static_cast<std::uint64_t>(previous.address) + previous.size;
      if (previous_end == dirty_begin) {
        previous.size +=
            static_cast<std::uint32_t>(dirty_end - dirty_begin);
        continue;
      }
    }
    result.ranges.push_back(
        WrittenRange{
            static_cast<std::uint32_t>(dirty_begin),
            static_cast<std::uint32_t>(dirty_end - dirty_begin)});
  }
  return result;
}

std::size_t AddressSpace::mapped_page_count() const {
  auto lock = read_lock();
  return vm_map_.page_count(page_size);
}

std::size_t AddressSpace::resident_page_count() const {
  auto lock = read_lock();
  return static_cast<std::size_t>(
      std::count_if(pages_->begin(), pages_->end(), [](const auto &entry) {
        return static_cast<bool>(entry.second.backing);
      }));
}

std::size_t AddressSpace::shared_page_count() const {
  auto lock = read_lock();
  const auto shared_metadata = !pages_.unique();
  return static_cast<std::size_t>(
      std::count_if(pages_->begin(), pages_->end(),
                    [shared_metadata](const auto &entry) {
        return entry.second.backing &&
               (shared_metadata || !entry.second.backing.unique());
      }));
}

std::size_t AddressSpace::cached_file_mapping_count() const {
  auto lock = read_lock();
  return static_cast<std::size_t>(
      std::count_if(pages_->begin(), pages_->end(), [](const auto &entry) {
        return entry.second.file_cached;
      }));
}

std::size_t AddressSpace::cached_file_page_count() const {
  return file_page_cache_->page_count();
}

std::size_t AddressSpace::mapping_region_count() const {
  auto lock = read_lock();
  return vm_map_.region_count();
}

std::optional<AddressSpace::MappingRegion>
AddressSpace::mapping_region_at_or_after(std::uint32_t address) const {
  auto lock = read_lock();
  return vm_map_.region_at_or_after(address);
}

std::unique_ptr<AddressSpace> AddressSpace::clone() const {
  auto result = std::make_unique<AddressSpace>();
  std::unique_lock source_lock{mutex_};
  std::unique_lock destination_lock{result->mutex_};
  result->vm_map_ = vm_map_;
  for (const auto &[address, page] : *pages_) {
    if (page.backing && !page.shared_writable) {
      page.copy_on_write_possible = true;
      const_cast<AddressSpace *>(this)->refresh_jit_page_locked(address);
    }
  }
  result->pages_ = pages_;
  result->file_mappings_ = file_mappings_;
  result->rebuild_page_lookup_locked();
  result->page_permissions_ = page_permissions_;
  result->tracked_write_ranges_ = tracked_write_ranges_;
  result->write_generation_ = write_generation_;
  result->mapping_leases_ = mapping_leases_;
  result->next_mapping_lease_token_ = next_mapping_lease_token_;
  result->file_page_cache_ = file_page_cache_;
  result->parallel_access_ = parallel_access_;
  result->jit_page_table_enabled_ =
      jit_page_table_enabled_ && !parallel_access_;
  return result;
}

void AddressSpace::add_page_permissions_locked(
    std::uint32_t address, std::uint64_t end,
    MemoryPermission permissions) {
  const auto bits = permission_bits(permissions);
  for (std::uint64_t base = address; base < end; base += page_size) {
    auto &chunk = writable_page_permission_chunk_locked(
        static_cast<std::size_t>(base / page_size));
    const auto index =
        static_cast<std::size_t>(base / page_size) %
        page_permission_chunk_size;
    chunk[index] |= static_cast<std::uint8_t>(mapped_page_flag | bits);
  }
}

void AddressSpace::set_page_permissions_locked(
    std::uint32_t address, std::uint64_t end,
    MemoryPermission permissions) {
  const auto flags = static_cast<std::uint8_t>(
      mapped_page_flag | permission_bits(permissions));
  for (std::uint64_t base = address; base < end; base += page_size) {
    auto &chunk = writable_page_permission_chunk_locked(
        static_cast<std::size_t>(base / page_size));
    chunk[static_cast<std::size_t>(base / page_size) %
          page_permission_chunk_size] = flags;
  }
}

void AddressSpace::clear_page_permissions_locked(std::uint32_t address,
                                                 std::uint64_t end) {
  for (std::uint64_t base = address; base < end; base += page_size) {
    const auto page_index =
        static_cast<std::size_t>(base / page_size);
    const auto chunk_index =
        page_index / page_permission_chunk_size;
    if (!page_permissions_[chunk_index]) continue;
    auto &chunk = writable_page_permission_chunk_locked(page_index);
    chunk[page_index % page_permission_chunk_size] = 0U;
  }
}

std::uint8_t
AddressSpace::page_permission_locked(std::size_t page_index) const {
  const auto &chunk =
      page_permissions_[page_index / page_permission_chunk_size];
  return chunk ? (*chunk)[page_index % page_permission_chunk_size] : 0U;
}

AddressSpace::PagePermissionChunk &
AddressSpace::writable_page_permission_chunk_locked(
    std::size_t page_index) {
  auto &chunk =
      page_permissions_[page_index / page_permission_chunk_size];
  if (!chunk) {
    chunk = std::make_shared<PagePermissionChunk>();
  } else if (!chunk.unique()) {
    chunk = std::make_shared<PagePermissionChunk>(*chunk);
  }
  return *chunk;
}

} // namespace ilemu
