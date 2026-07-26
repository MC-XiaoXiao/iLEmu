#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

#include "ilemu/memory_permission.hpp"

namespace ilemu {

// Compact, non-overlapping vm_map-style metadata. Callers retain their own
// synchronization; this class only owns mapping and protection intervals.
class VmMap {
public:
  struct MappingRegion {
    std::uint32_t address{};
    std::uint64_t end{};
    MemoryPermission permissions{MemoryPermission::None};
  };

  void map_or(std::uint32_t start, std::uint64_t end,
              MemoryPermission permissions);
  void unmap(std::uint32_t start, std::uint64_t end);
  [[nodiscard]] bool protect(std::uint32_t start, std::uint64_t end,
                             MemoryPermission permissions);

  [[nodiscard]] bool accessible(std::uint32_t start, std::uint64_t end,
                                MemoryPermission access) const;
  [[nodiscard]] bool overlaps(std::uint32_t start, std::uint64_t end) const;
  [[nodiscard]] std::optional<MappingRegion>
  region_at_or_after(std::uint32_t address) const;
  [[nodiscard]] std::size_t page_count(std::uint32_t page_size) const;
  [[nodiscard]] std::size_t region_count() const { return regions_.size(); }
  void clear() { regions_.clear(); }

private:
  struct Region {
    std::uint64_t end{};
    MemoryPermission permissions{MemoryPermission::None};
  };

  void split_at(std::uint64_t point);
  void coalesce();

  std::map<std::uint32_t, Region> regions_;
};

} // namespace ilemu
