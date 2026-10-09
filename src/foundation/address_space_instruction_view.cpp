/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/address_space.hpp"
#include <stdexcept>

namespace ilemu {
std::optional<std::shared_ptr<GuestPageBacking>>
AddressSpace::instruction_page_backing(std::uint32_t address)
{
    if (!owns_exclusive_access())
        throw std::logic_error(
            "instruction backing requires an address-space lease");
    const auto base = address & ~(page_size - 1U);
    if (!range_accessible_locked(base, page_size, MemoryPermission::Execute) ||
        (vm_translation_locked(base / page_size) &
            static_cast<std::uint8_t>(MemoryPermission::Execute)) == 0U)
        return std::nullopt;
    auto* page = find_page_locked(base);
    if (!page || !page->backing)
        return std::shared_ptr<GuestPageBacking> { };
    if (page->shared_writable) {
        const auto epoch = GuestPageBacking::shared_write_tracking_epoch();
        const auto changed = page->backing->enable_shared_write_tracking();
        finish_shared_write_tracking_locked(epoch, changed ? 1U : 0U, true);
    }
    return page->backing;
}
}
