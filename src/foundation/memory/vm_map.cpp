#include "ilemu/vm_map.hpp"

#include <algorithm>
#include <iterator>
#include <limits>

namespace ilemu {
namespace {

constexpr std::uint64_t address_space_end =
    std::uint64_t{1} << std::numeric_limits<std::uint32_t>::digits;

[[nodiscard]] bool valid_range(std::uint32_t start, std::uint64_t end) {
  return end > start && end <= address_space_end;
}

} // namespace

void VmMap::split_at(std::uint64_t point) {
  if (point == 0 || point >= address_space_end || regions_.empty()) return;
  const auto after = regions_.upper_bound(static_cast<std::uint32_t>(point));
  if (after == regions_.begin()) return;
  const auto containing = std::prev(after);
  if (point <= containing->first || point >= containing->second.end) return;
  const auto previous_end = containing->second.end;
  const auto permissions = containing->second.permissions;
  containing->second.end = point;
  regions_.emplace(static_cast<std::uint32_t>(point),
                   Region{previous_end, permissions});
}

void VmMap::coalesce() {
  for (auto current = regions_.begin(); current != regions_.end();) {
    const auto next = std::next(current);
    if (next == regions_.end()) break;
    if (current->second.end == next->first &&
        current->second.permissions == next->second.permissions) {
      current->second.end = next->second.end;
      regions_.erase(next);
    } else {
      current = next;
    }
  }
}

void VmMap::map_or(std::uint32_t start, std::uint64_t end,
                   MemoryPermission permissions) {
  if (!valid_range(start, end)) return;
  split_at(start);
  split_at(end);

  std::uint64_t cursor = start;
  while (cursor < end) {
    auto region = regions_.lower_bound(static_cast<std::uint32_t>(cursor));
    if (region == regions_.end() || region->first > cursor) {
      const auto gap_end =
          region == regions_.end()
              ? end
              : std::min<std::uint64_t>(end, region->first);
      regions_.emplace(static_cast<std::uint32_t>(cursor),
                       Region{gap_end, permissions});
      cursor = gap_end;
      continue;
    }
    region->second.permissions |= permissions;
    cursor = region->second.end;
  }
  coalesce();
}

void VmMap::unmap(std::uint32_t start, std::uint64_t end) {
  if (!valid_range(start, end)) return;
  split_at(start);
  split_at(end);
  auto region = regions_.lower_bound(start);
  while (region != regions_.end() && region->first < end) {
    region = regions_.erase(region);
  }
  coalesce();
}

bool VmMap::protect(std::uint32_t start, std::uint64_t end,
                    MemoryPermission permissions) {
  if (!accessible(start, end, MemoryPermission::None)) return false;
  split_at(start);
  split_at(end);
  for (auto region = regions_.lower_bound(start);
       region != regions_.end() && region->first < end; ++region) {
    region->second.permissions = permissions;
  }
  coalesce();
  return true;
}

bool VmMap::accessible(std::uint32_t start, std::uint64_t end,
                       MemoryPermission access) const {
  if (!valid_range(start, end)) return false;
  auto after = regions_.upper_bound(start);
  if (after == regions_.begin()) return false;
  auto region = std::prev(after);
  std::uint64_t cursor = start;
  while (cursor < end) {
    if (region == regions_.end() || region->first > cursor ||
        region->second.end <= cursor ||
        !has_permission(region->second.permissions, access)) {
      return false;
    }
    cursor = region->second.end;
    if (cursor >= end) return true;
    ++region;
  }
  return true;
}

bool VmMap::overlaps(std::uint32_t start, std::uint64_t end) const {
  if (!valid_range(start, end)) return false;
  auto region = regions_.lower_bound(start);
  if (region != regions_.end() && region->first < end) return true;
  if (region == regions_.begin()) return false;
  --region;
  return region->second.end > start;
}

std::size_t VmMap::page_count(std::uint32_t page_size) const {
  std::uint64_t count = 0;
  for (const auto &[start, region] : regions_) {
    count += (region.end - start) / page_size;
  }
  return static_cast<std::size_t>(count);
}

} // namespace ilemu
