/* SPDX-License-Identifier: MPL-2.0 */
#include "address_cache.hpp"
#include "native_frame.hpp"
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
AddressCache::AddressCache(VectorCodeGenerator& code, bool extra_slots_available)
    : code_(code), slot_count_(extra_slots_available ? 4U : 2U)
{
}
std::optional<XReg> AddressCache::find(const Key& key) const
{
    for (unsigned slot = 0; slot < slot_count_; ++slot) {
        if (entries_[slot] != key)
            continue;
        if (slot < 2)
            return integer_register(slot);
        code_.FMOV(X21, spill_register(slot));
        return X21;
    }
    return {};
}
void AddressCache::begin_guard(const Key& key)
{
    const auto slot = next_ % slot_count_;
    if (first_[slot])
        return;
    first_[slot] = Guard { key, code_.offset(), 0 };
    code_.NOP();
    if (slot >= 2)
        code_.NOP();
}
XReg AddressCache::remember(const Key& key, XReg pointer)
{
    const auto slot = next_++ % slot_count_;
    entries_[slot] = key;
    XReg result = X21;
    if (slot < 2) {
        result = integer_register(slot);
        code_.MOV(result, pointer);
    } else {
        used_extra_ = true;
        if (pointer.index() != X21.index())
            code_.MOV(X21, pointer);
        code_.FMOV(spill_register(slot), X21);
    }
    if (auto& first = first_[slot];
        first && first->target == 0 && first->key == key)
        first->target = code_.offset();
    return result;
}
void AddressCache::invalidate(unsigned reg)
{
    // Writes before first publication and conditional writes also revoke the
    // cross-iteration proof. No cached pointer survives a checked/native exit.
    written_ |= 1U << reg;
    for (auto& entry : entries_)
        if (entry && (entry->inputs() & (1U << reg)) != 0)
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
        if (!first || first->target == 0 || entries_[slot] != first->key ||
            (written_ & first->key.inputs()) != 0)
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
