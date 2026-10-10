/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/address_space.hpp"

namespace ilemu {
std::optional<execution::ExclusiveReservation>
AddressSpace::load_exclusive(std::uint32_t address, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        (address & (size - 1U)) != 0)
        return std::nullopt;
    ExclusiveAccess access { *this };
    account_vm_access_locked(address, size, MemoryPermission::Read);
    if (!range_accessible_locked(address, size, MemoryPermission::Read))
        return std::nullopt;
    auto& page = ensure_page_locked(address);
    // Resolve lazy/zero/COW storage before publishing the stamp, just as the
    // existing shared synchronization-identity path does. No guest write.
    bool changed = false;
    auto& backing = writable_backing_locked(page, &changed);
    if (changed) {
        refresh_jit_page_locked(address);
        bump_executable_content_generation_locked();
    }
    if (page.shared_writable && !backing.shared_write_tracking_enabled()) {
        const auto epoch = GuestPageBacking::shared_write_tracking_epoch();
        const auto enabled = backing.enable_shared_write_tracking();
        finish_shared_write_tracking_locked(epoch, enabled ? 1U : 0U, true);
    }
    track_exclusive_access(address, size);
    const auto physical = backing.lock_instruction_read();
    execution::ExclusiveReservation result;
    result.size = size;
    result.offset = address & (guest_exclusive_granule_size - 1U);
    const auto offset = address & (page_size - 1U);
    for (unsigned i = 0; i < size; ++i)
        result.value |= std::uint64_t { std::to_integer<std::uint8_t>(
                            backing.bytes[offset + i]) } << (i * 8U);
    result.key = exclusive_reservation_key(address);
    // Never accept the legacy virtual fallback as a physical reservation.
    if ((result.key & (std::uint64_t { 1 } << 63U)) == 0)
        result.key = 0;
    return result;
}
}
