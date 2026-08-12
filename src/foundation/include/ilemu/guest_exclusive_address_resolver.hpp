#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <shared_mutex>

#include <dynarmic/interface/exclusive_monitor.h>

#include "ilemu/address_space.hpp"

namespace ilemu {

// Maps Dynarmic's globally unique processor slots back to the AddressSpace
// that owns the Guest virtual address. The resolver is shared by all runtime
// clusters that use one process-wide ExclusiveMonitor.
class GuestExclusiveAddressResolver {
public:
  void bind(std::size_t processor_base, std::size_t processor_count,
            AddressSpace &memory);
  void unbind(std::size_t processor_base, std::size_t processor_count,
              AddressSpace &memory) noexcept;

  [[nodiscard]] Dynarmic::VAddr resolve(
      std::size_t processor_id, Dynarmic::VAddr address) const noexcept;

  static Dynarmic::VAddr resolve_callback(
      void *context, std::size_t processor_id,
      Dynarmic::VAddr address) noexcept;

private:
  struct Binding {
    std::size_t end{};
    AddressSpace *memory{};
  };

  mutable std::shared_mutex mutex_;
  std::map<std::size_t, Binding> bindings_;
};

} // namespace ilemu
