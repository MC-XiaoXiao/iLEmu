/* SPDX-License-Identifier: MPL-2.0 */
#include "address_cache.hpp"
#include "native_frame.hpp"
#include "address_windows.hpp"
#include "flag_effects.hpp"
#include "arm/integer_semantics.hpp"
namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
namespace {
std::uint16_t condition_coverage(unsigned condition)
{
    static const auto masks = [] {
        std::array<std::uint16_t, 16> result { };
        for (unsigned condition = 0; condition < result.size(); ++condition)
            for (unsigned flags = 0; flags < 16; ++flags)
                if (arm::condition_passed(condition, flags << 28U))
                    result[condition] |= 1U << flags;
        return result;
    }();
    return masks[condition];
}
bool preserves_flags(const arm::Instruction& inst)
{
    const auto effects = flag_effects(inst);
    if (effects.possible_write != 0)
        return false;
    if (!effects.boundary)
        return true;
    using Kind = arm::InstructionKind;
    switch (inst.kind) {
    case Kind::Transfer:
    case Kind::DualTransfer:
    case Kind::MultipleTransfer:
    case Kind::VectorTransfer:
    case Kind::ExclusiveTransfer:
    case Kind::Swap:
    case Kind::ClearExclusive:
        return true;
    default:
        return false;
    }
}
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
    const AddressWindows* windows, bool conditional,
    std::pmr::memory_resource* temporary)
    : code_(code), late_(temporary), windows_(windows),
      displacements_(windows ? std::make_optional<AddressDisplacements>() : std::nullopt),
      slot_count_(extra_slots_available ? 4U : 2U), conditional_(conditional)
{
}
AddressCache::Identity AddressCache::identity(const Key& key) const
{
    if (windows_ && displacements_->get(key.base))
        if (auto window = windows_->find(key))
            return *window;
    return key;
}
std::uint32_t AddressCache::inputs(const Identity& id) const
{
    if (const auto* key = std::get_if<Key>(&id))
        return key->inputs();
    // Window slots anchor the entry base, even across proven temporary writes.
    return 0;
}
AddressCache::Reference AddressCache::reference(
    const Key& key, const Identity& id, XReg base) const
{
    if (std::holds_alternative<Key>(id))
        return { base };
    const auto displacement = displacements_->get(key.base).value();
    if (key.index) {
        if (displacement != 0)
            base = Reference { base, displacement }.immediate(code_);
        return { base, 0, true };
    }
    return { base, static_cast<std::int32_t>(displacement +
        (key.add ? std::int64_t { key.offset } : -std::int64_t { key.offset })) };
}
std::optional<AddressCache::Reference> AddressCache::find(const Key& key) const
{
    const auto id = identity(key);
    for (unsigned slot = 0; slot < slot_count_; ++slot) {
        if (!entries_[slot] || !entries_[slot]->dominates || entries_[slot]->identity != id)
            continue;
        if (slot < 2)
            return reference(key, id, integer_register(slot));
        code_.FMOV(X21, spill_register(slot));
        return reference(key, id, X21);
    }
    return {};
}
unsigned AddressCache::slot(const Identity& id) const
{
    if (!coverage_used_)
        return next_ % slot_count_;
    // Reuse a conditional publication before spending another bounded slot.
    for (unsigned slot = 0; slot < slot_count_; ++slot)
        if (entries_[slot] && !entries_[slot]->dominates &&
            entries_[slot]->identity == id)
            return slot;
    return next_ % slot_count_;
}
void AddressCache::begin_guard(const Key& key, unsigned condition)
{
    if (condition != 14 && !conditional_)
        return;
    const auto id = identity(key);
    const auto selected = slot(id);
    pending_ = selected;
    if (!first_[selected]) {
        first_[selected] = Guard { id, code_.offset(), 0 };
    } else if (entries_[selected] && !entries_[selected]->dominates &&
        entries_[selected]->identity == id && !mixed_[selected]) {
        late_.push_back({ selected, code_.offset() });
    } else
        return;
    code_.NOP();
    if (selected >= 2)
        code_.NOP();
}
AddressCache::Reference AddressCache::remember(
    const Key& key, XReg pointer, unsigned condition)
{
    if (condition != 14 && !conditional_)
        return { pointer };
    const auto slot = *pending_;
    pending_.reset();
    const auto id = identity(key);
    auto& entry = entries_[slot];
    if (!entry || entry->identity != id) {
        entry = Entry { id };
        ++next_;
    }
    mixed_[slot] = mixed_[slot] || first_[slot]->identity != id;
    entry->coverage |= condition == 14 ? UINT16_MAX : condition_coverage(condition);
    entry->dominates = entry->coverage == UINT16_MAX;
    coverage_used_ |= condition != 14;
    const auto result = slot < 2 ? integer_register(slot) : X21;
    if (std::holds_alternative<unsigned>(id)) {
        // The selector proved every planned offset stays in this same page.
        // The original instruction just validated its read/write page view.
        code_.SUB(result, pointer, X15);
        const auto displacement = displacements_->get(key.base).value();
        if (displacement == 0)
            code_.ADD(result, result, XReg { static_cast<int>(key.base) });
        else {
            // Reconstruct the entry base modulo32 before host-pointer addition.
            // A temporary wrapped base can still have a page-local access.
            code_.MOV(W17, static_cast<std::uint32_t>(displacement));
            code_.SUB(W17, WReg { static_cast<int>(key.base) }, W17);
            code_.ADD(result, result, X17);
        }
    } else if (result.index() != pointer.index())
        code_.MOV(result, pointer);
    if (slot >= 2) {
        used_extra_ = true;
        code_.FMOV(spill_register(slot), result);
    }
    if (auto& first = first_[slot];
        first && first->target == 0 && first->identity == id)
        first->target = code_.offset();
    if (!late_.empty() && late_.back().target == 0 && late_.back().slot == slot)
        late_.back().target = code_.offset();
    return reference(key, id, result);
}
void AddressCache::observe(const arm::Instruction& inst)
{
    for_written_register(inst, [&](unsigned reg) {
        written_ |= 1U << reg;
        for (auto& entry : entries_)
            if (entry && (inputs(entry->identity) & (1U << reg)) != 0)
                entry.reset();
    });
    // Coverage refers to one NZCV snapshot. Once established, dominance
    // survives flag writes, but partial predicate coverage cannot cross them.
    if (coverage_used_ && !preserves_flags(inst))
        for (auto& entry : entries_)
            if (entry && !entry->dominates)
                entry->coverage = 0;
    if (displacements_)
        displacements_->observe(inst);
}
void AddressCache::writeback(const arm::Instruction& inst)
{
    if (!inst.writeback)
        return;
    const auto base = WReg { static_cast<int>(inst.rn) };
    const auto known = AddressDisplacements::writeback(inst);
    if (!known) {
        code_.MOV(base, W16);
        return;
    }
    const auto change = *known;
    const auto magnitude = static_cast<std::uint64_t>(change.delta < 0
        ? -std::int64_t { change.delta } : std::int64_t { change.delta });
    if (AddSubImm::is_valid(magnitude)) {
        if (change.delta < 0)
            code_.SUB(base, base, AddSubImm { magnitude });
        else
            code_.ADD(base, base, AddSubImm { magnitude });
    } else {
        code_.MOV(W16, static_cast<std::uint32_t>(change.delta));
        code_.ADD(base, base, W16);
    }
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
        if (!first || first->target == 0 || (!entries_[slot] || entries_[slot]->identity != first->identity) ||
            (written_ & inputs(first->identity)) != 0)
            continue;
        if (const auto* window = std::get_if<unsigned>(&first->identity);
            window && displacements_->get(windows_->window(*window).base) != 0)
            continue;
        const bool patch_first = entries_[slot]->dominates || !mixed_[slot];
        if (!patch_first)
            continue;
        // Slots start empty on each invocation. A conditional instruction may
        // be the first executed access; it must still validate at its own PC.
        if (slot < 2)
            entry.MOV(integer_register(slot), 0);
        else
            entry.FMOV(spill_register(slot), XZR);
        const auto patch_guard = [&](std::ptrdiff_t branch, std::ptrdiff_t target) {
            CodeGenerator patch(words.data() + branch / 4);
            if (slot < 2)
                patch.CBNZ(integer_register(slot), target - branch);
            else {
                patch.FMOV(X21, spill_register(slot));
                patch.CBNZ(X21, target - branch - 4);
            }
        };
        patch_guard(first->branch, first->target);
        // Across a mixed slot, nonzero alone cannot identify its pointer.
        if (!mixed_[slot])
            for (const auto& guard : late_)
                if (guard.slot == slot && guard.target != 0)
                    patch_guard(guard.branch, guard.target);
    }
}
}
