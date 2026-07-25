#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "ilegacysim/file_page_cache.hpp"
#include "ilegacysim/memory_permission.hpp"
#include "ilegacysim/vm_map.hpp"

namespace ilegacysim {

struct MemoryFault {
  std::uint32_t address{};
  std::size_t size{};
  MemoryPermission access{MemoryPermission::None};
  std::string message;
};

class AddressSpace {
public:
  static constexpr std::uint32_t page_size = guest_memory_page_size;
  static constexpr std::size_t page_count =
      (std::uint64_t{1} << 32U) / page_size;

  AddressSpace();
  ~AddressSpace();

  // Selects the synchronization policy before guest execution starts. The
  // physical single-core device runs all memory access on the scheduler
  // thread; optional multi-core sessions retain shared/exclusive locking.
  void set_parallel_access(bool enabled);
  // Dynarmic may directly access resident private/shared-writable pages while
  // the physical single-core model serializes all guest execution. Unsafe
  // pages remain null and transparently fall back to the checked callbacks.
  // The returned table has one host page pointer per 4 KiB guest page and
  // remains valid for this AddressSpace's lifetime.
  [[nodiscard]] std::uint8_t **jit_page_table();
  // Debug write watchpoints and parallel virtual-CPU sessions require every
  // access to pass through callbacks. Existing JITs retain a valid all-null
  // table after this call.
  void disable_jit_page_table();

  bool map(std::uint32_t address, std::uint32_t size,
           MemoryPermission permissions);
  bool unmap(std::uint32_t address, std::uint32_t size);
  void clear();
  bool protect(std::uint32_t address, std::uint32_t size,
               MemoryPermission permissions);
  bool copy_in(std::uint32_t address, std::span<const std::byte> data);
  [[nodiscard]] bool copy_out(std::uint32_t address,
                              std::span<std::byte> data) const;
  // Installs page-aligned immutable file backing. A guest write automatically
  // detaches that page, providing MAP_PRIVATE/shared-region COW semantics.
  bool map_file(std::uint32_t address, std::uint32_t size,
                MemoryPermission permissions,
                const std::filesystem::path &path,
                std::uint64_t file_offset);
  enum class PageMappingMode {
    CopyOnWrite,
    Shared,
  };
  // Exposes an existing page-aligned range as a Mach named-memory object.
  // Existing fork/file-cache COW backings are detached first; subsequent
  // mappings of the returned pages observe shared writes like XNU vm_map.
  [[nodiscard]] std::optional<
      std::vector<std::shared_ptr<GuestPageBacking>>>
  share_pages(std::uint32_t address, std::uint32_t size);
  // Installs backings owned by a Mach named-memory object. CopyOnWrite keeps
  // vm_map(copy=TRUE) private while Shared preserves cross-mapping writes.
  bool map_page_backings(
      std::uint32_t address, std::uint32_t size,
      MemoryPermission permissions,
      std::span<const std::shared_ptr<GuestPageBacking>> backings,
      PageMappingMode mode);
  [[nodiscard]] std::optional<std::vector<std::byte>>
  read_bytes(std::uint32_t address, std::size_t size) const;
  [[nodiscard]] std::optional<std::string>
  read_c_string(std::uint32_t address, std::size_t maximum_size = 4096) const;

  [[nodiscard]] std::optional<std::uint8_t>
  read8(std::uint32_t address,
        MemoryPermission access = MemoryPermission::Read) const;
  [[nodiscard]] std::optional<std::uint16_t>
  read16(std::uint32_t address,
         MemoryPermission access = MemoryPermission::Read) const;
  [[nodiscard]] std::optional<std::uint32_t>
  read32(std::uint32_t address,
         MemoryPermission access = MemoryPermission::Read) const;
  [[nodiscard]] std::optional<std::uint64_t>
  read64(std::uint32_t address,
         MemoryPermission access = MemoryPermission::Read) const;

  bool write8(std::uint32_t address, std::uint8_t value);
  bool write16(std::uint32_t address, std::uint16_t value);
  bool write32(std::uint32_t address, std::uint32_t value);
  bool write64(std::uint32_t address, std::uint64_t value);

  [[nodiscard]] bool accessible(std::uint32_t address, std::size_t size,
                                MemoryPermission access) const;
  bool compare_exchange8(std::uint32_t address, std::uint8_t expected,
                         std::uint8_t value);
  bool compare_exchange16(std::uint32_t address, std::uint16_t expected,
                          std::uint16_t value);
  bool compare_exchange32(std::uint32_t address, std::uint32_t expected,
                          std::uint32_t value);
  bool compare_exchange64(std::uint32_t address, std::uint64_t expected,
                          std::uint64_t value);
  [[nodiscard]] std::optional<std::uint8_t>
  exchange8(std::uint32_t address, std::uint8_t value);
  [[nodiscard]] std::optional<std::uint32_t>
  exchange32(std::uint32_t address, std::uint32_t value);

  [[nodiscard]] bool mapped(std::uint32_t address, std::size_t size = 1) const;
  // Write generations are only needed by memory consumers that poll a
  // specific range (for example, a legacy scanout buffer). Registering that
  // range keeps ordinary Guest stores out of the dirty-generation path.
  bool track_write_generation(std::uint32_t address, std::size_t size);
  // Returns the newest write generation among pages intersecting the range.
  // Graphics scanout uses this to avoid copying an unchanged framebuffer.
  [[nodiscard]] std::optional<std::uint64_t> range_write_generation(
      std::uint32_t address, std::size_t size) const;
  [[nodiscard]] std::size_t mapped_page_count() const;
  // Demand-zero mappings do not become resident until their first write.
  [[nodiscard]] std::size_t resident_page_count() const;
  // Resident backings shared by fork clones or the immutable file cache.
  [[nodiscard]] std::size_t shared_page_count() const;
  [[nodiscard]] std::size_t cached_file_mapping_count() const;
  [[nodiscard]] std::size_t cached_file_page_count() const;
  [[nodiscard]] std::size_t mapping_region_count() const;
  [[nodiscard]] std::unique_ptr<AddressSpace> clone() const;

private:
  struct Page {
    std::shared_ptr<GuestPageBacking> backing;
    std::uint64_t write_generation{};
    bool file_cached{};
    bool shared_writable{};
    // Avoid an atomic shared_ptr use-count read on every guest store. This is
    // set only when fork/file/private-object sharing can require a detach.
    mutable bool copy_on_write_possible{};
  };
  struct FileMapping {
    std::uint64_t end{};
    std::uint64_t file_offset{};
    std::shared_ptr<GuestFileBacking> backing;
  };
  struct TrackedWriteRange {
    std::uint32_t begin{};
    std::uint64_t end{};
  };
  struct JitPageTableStorage;
  static constexpr std::size_t page_lookup_chunk_size = 1024;
  static constexpr std::size_t page_lookup_chunk_count =
      page_count / page_lookup_chunk_size;
  using PageLookupChunk = std::array<Page *, page_lookup_chunk_size>;
  using ReadLock = std::shared_lock<std::shared_mutex>;
  using WriteLock = std::unique_lock<std::shared_mutex>;

  template <typename T>
  [[nodiscard]] std::optional<T> read_integer(std::uint32_t address,
                                              MemoryPermission access) const;
  template <typename T> bool write_integer(std::uint32_t address, T value);
  template <typename T>
  bool compare_exchange_integer(std::uint32_t address, T expected, T value);
  template <typename T>
  [[nodiscard]] std::optional<T> exchange_integer(std::uint32_t address,
                                                  T value);

  [[nodiscard]] bool range_accessible_locked(std::uint32_t address,
                                             std::size_t size,
                                             MemoryPermission access) const;
  [[nodiscard]] const Page *find_page_locked(std::uint32_t address) const;
  [[nodiscard]] Page *find_page_locked(std::uint32_t address);
  [[nodiscard]] const FileMapping *
  find_file_mapping_locked(std::uint32_t address) const;
  [[nodiscard]] Page &ensure_page_locked(std::uint32_t address);
  [[nodiscard]] bool range_needs_file_fault_locked(
      std::uint32_t address, std::size_t size) const;
  [[nodiscard]] bool fault_file_pages(std::uint32_t address,
                                      std::size_t size);
  void unmap_file_mappings_locked(std::uint32_t address, std::uint64_t end);
  void cache_page_locked(std::uint32_t address, Page &page);
  void uncache_page_locked(std::uint32_t address);
  void rebuild_page_lookup_locked();
  void refresh_jit_page_locked(std::uint32_t address);
  void refresh_jit_page_range_locked(std::uint32_t address,
                                     std::uint64_t end);
  void clear_jit_page_table_locked();
  [[nodiscard]] static std::byte read_byte_locked(const Page *page,
                                                  std::uint32_t offset);
  [[nodiscard]] static GuestPageBacking &writable_backing_locked(Page &page);
  [[nodiscard]] bool tracks_write_locked(std::uint32_t address,
                                         std::size_t size) const;
  void mark_written_locked(std::uint32_t address, std::size_t size);
  void add_page_permissions_locked(std::uint32_t address, std::uint64_t end,
                                   MemoryPermission permissions);
  void set_page_permissions_locked(std::uint32_t address, std::uint64_t end,
                                   MemoryPermission permissions);
  void clear_page_permissions_locked(std::uint32_t address,
                                     std::uint64_t end);
  [[nodiscard]] ReadLock read_lock() const;
  [[nodiscard]] WriteLock write_lock();

  mutable std::shared_mutex mutex_;
  bool parallel_access_{true};
  VmMap vm_map_;
  std::map<std::uint32_t, Page> pages_;
  // File-backed vm_map entries remain range metadata until a guest access
  // faults an individual page into pages_. This mirrors XNU's vnode pager and
  // avoids constructing thousands of page objects during mmap.
  std::map<std::uint32_t, FileMapping> file_mappings_;
  // Sparse two-level lookup for resident pages. Tree ownership preserves
  // efficient range unmap while CPU callbacks avoid a tree search.
  std::array<std::unique_ptr<PageLookupChunk>, page_lookup_chunk_count>
      page_lookup_{};
  // The interval map remains the source of truth for VM operations. This dense
  // byte index is kept in sync so the per-instruction CPU callbacks can check
  // the overwhelmingly common one-page access without a tree lookup.
  std::vector<std::uint8_t> page_permissions_;
  std::unique_ptr<JitPageTableStorage> jit_page_table_;
  bool jit_page_table_enabled_{};
  std::optional<TrackedWriteRange> tracked_write_range_;
  std::uint64_t write_generation_{};
  std::shared_ptr<FilePageCache> file_page_cache_;
};

} // namespace ilegacysim
