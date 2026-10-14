/* SPDX-License-Identifier: MPL-2.0 */
#include "address_cache.hpp"
#include "native_frame.hpp"
#include "address_windows.hpp"
namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
namespace {
DReg spill_register(unsigned slot)
{
    return DReg { 8 + static_cast<int>(slot - 2U) };
}
XReg integer_register(unsigned slot)
{
    return XReg { 27 + static_cast<int>(slot) };
}
}
XReg AddressCache::Reference::immediate(VectorCodeGenerator& code) const
{
    if (offset == 0)
        return base;
    if (offset > 0)
        code.ADD(X21, base, static_cast<unsigned>(offset));
    else
        code.SUB(X21, base, static_cast<unsigned>(-offset));
    return X21;
}
AddressCache::AddressCache(VectorCodeGenerator& code, bool extra_slots_available,
    const AddressWindows* windows)
    : code_(code), windows_(windows), slot_count_(extra_slots_available ? 4U : 2U)
{
}
AddressCache::Identity AddressCache::identity(const Key& key) const
{
    if (windows_)
        if (auto window = windows_->find(key))
            return *window;
    return key;
}
std::uint32_t AddressCache::inputs(const Identity& id) const
{
    if (const auto* key = std::get_if<Key>(&id))
        return key->inputs();
    return 1U << windows_->window(std::get<unsigned>(id)).base;
}
AddressCache::Reference AddressCache::reference(
    const Key& key, const Identity& id, XReg base) const
{
    if (std::holds_alternative<Key>(id))
        return { base };
    return { base, key.index ? 0 : static_cast<std::int32_t>(
        key.add ? std::int64_t { key.offset } : -std::int64_t { key.offset }),
        key.index.has_value() };
}
std::optional<AddressCache::Reference> AddressCache::find(const Key& key) const
{
    const auto id = identity(key);
    for (unsigned slot = 0; slot < slot_count_; ++slot) {
        if (!entries_[slot] || *entries_[slot] != id)
            continue;
        if (slot < 2)
            return reference(key, id, integer_register(slot));
        code_.FMOV(X21, spill_register(slot));
        return reference(key, id, X21);
    }
    return {};
}
void AddressCache::begin_guard(const Key& key)
{
    const auto slot = next_ % slot_count_;
    if (first_[slot])
        return;
    first_[slot] = Guard { identity(key), code_.offset(), 0 };
    code_.NOP();
    if (slot >= 2)
        code_.NOP();
}
AddressCache::Reference AddressCache::remember(const Key& key, XReg pointer)
{
    const auto slot = next_++ % slot_count_;
    const auto id = identity(key);
    entries_[slot] = id;
    const auto result = slot < 2 ? integer_register(slot) : X21;
    if (std::holds_alternative<unsigned>(id)) {
        // The selector proved every planned offset stays in this same page.
        // The original instruction just validated its read/write page view.
        code_.SUB(result, pointer, X15);
        code_.ADD(result, result, XReg { static_cast<int>(key.base) });
    } else if (result.index() != pointer.index())
        code_.MOV(result, pointer);
    if (slot >= 2) {
        used_extra_ = true;
        code_.FMOV(spill_register(slot), result);
    }
    if (auto& first = first_[slot];
        first && first->target == 0 && first->identity == id)
        first->target = code_.offset();
    return reference(key, id, result);
}
void AddressCache::invalidate(unsigned reg)
{
    // Writes before first publication and conditional writes also revoke the
    // cross-iteration proof. No cached pointer survives a checked/native exit.
    written_ |= 1U << reg;
    for (auto& entry : entries_)
        if (entry && (inputs(*entry) & (1U << reg)) != 0)
            entry.reset();
}
void AddressCache::clear()
{
    entries_ = {};
    written_ = 0xffffU;
}
void AddressCache::enter(VectorCodeGenerator& code) const
{
    if (used_extra_)
        code.STP(D8, D9, SP, NativeFrame::memory_cache_offset);
}
void AddressCache::leave() const
{
    if (used_extra_)
        code_.LDP(D8, D9, SP, NativeFrame::memory_cache_offset);
}
void AddressCache::finalize(bool closed, std::vector<std::uint32_t>& words,
    VectorCodeGenerator& entry) const
{
    if (!closed)
        return;
    for (unsigned slot = 0; slot < slot_count_; ++slot) {
        const auto& first = first_[slot];
        if (!first || first->target == 0 || (!entries_[slot] || *entries_[slot] != first->identity) ||
            (written_ & inputs(first->identity)) != 0)
            continue;
        // First execution still validates at the original instruction PC.
        // Backedges alone skip to its access, with the saved pointer ready.
        CodeGenerator patch(words.data() + first->branch / 4);
        if (slot < 2) {
            const auto reg = integer_register(slot);
            entry.MOV(reg, 0);
            patch.CBNZ(reg, first->target - first->branch);
        } else {
            entry.FMOV(spill_register(slot), XZR);
            patch.FMOV(X21, spill_register(slot));
            patch.CBNZ(X21, first->target - first->branch - 4);
        }
    }
}
}
