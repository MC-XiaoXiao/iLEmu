/* SPDX-License-Identifier: MPL-2.0 */
#include "address_windows.hpp"
#include "arm/integer_semantics.hpp"
#include "register_writes.hpp"
#include <algorithm>
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
namespace {
    bool fits(std::int64_t first, std::int64_t end)
    {
        return first < end && end - first <= 4096 &&
               std::max<std::int64_t>(0, -first) <=
                   std::min<std::int64_t>(4095, 4096 - end);
    }
}
void AddressWindows::observe(
    const arm::Instruction& inst, const std::optional<Key>& key)
{
    if (key && key->base < 15 && !key->pc_base) {
        auto found = std::find_if(accesses_.begin(), accesses_.end(),
            [&](const Access& access) { return access.key == *key; });
        if (found == accesses_.end()) {
            accesses_.push_back(Access { *key });
            found = std::prev(accesses_.end());
        }
        auto& access = *found;
        access.unconditional |= inst.condition == 14;
        std::int64_t low =
            key->add ? key->offset : -std::int64_t { key->offset };
        auto high = low;
        const auto alignment = std::max(key->alignment, key->natural_alignment);
        bool valid = true;
        if (key->index) {
            const auto mask = bits_[*key->index];
            valid = mask.has_value() && key->offset == 0;
            if (valid && key->shift == arm::ShiftKind::Lsl)
                valid = key->shift_amount < 32 &&
                        (std::uint64_t { *mask } << key->shift_amount) <=
                            UINT32_MAX;
            if (valid) {
                const auto shifted = arm::shift(
                    *mask, key->shift, key->shift_amount, false, false)
                                         .value;
                valid = (shifted & (alignment - 1U)) == 0;
                low = key->add ? 0 : -std::int64_t { shifted };
                high = key->add ? shifted : 0;
            }
        }
        high += key->size;
        valid &= fits(low, high);
        if (access.valid && valid) {
            if (access.end == 0 && access.first == 0) {
                access.first = low;
                access.end = high;
            } else {
                access.first = std::min(access.first, low);
                access.end = std::max(access.end, high);
            }
            access.residue = static_cast<unsigned>(low) & (alignment - 1U);
        }
        access.valid &= valid;
    }
    std::optional<std::uint32_t> result;
    if (inst.kind == arm::InstructionKind::DataProcessing &&
        inst.condition == 14 && inst.immediate_operand) {
        if (inst.opcode == 0 || inst.opcode == 13)
            result = inst.immediate;
        else if (inst.opcode == 14 || inst.opcode == 15)
            result = ~inst.immediate;
    }
    for_written_register(inst, [&](unsigned reg) {
        bits_[reg].reset();
        written_ |= 1U << reg;
    });
    if (result)
        bits_[inst.rd] = result;
}
void AddressWindows::finish(unsigned slots)
{
    for (auto& access : accesses_) {
        const auto& key = access.key;
        if (!access.valid || (written_ & (1U << key.base)) != 0)
            continue;
        auto found = std::find_if(
            windows_.begin(), windows_.end(), [&](const Window& w) {
                return w.base == key.base && w.load == key.load &&
                       w.alignment == key.alignment &&
                       w.natural_alignment == key.natural_alignment &&
                       w.residue == access.residue;
            });
        if (found == windows_.end()) {
            windows_.push_back(
                Window { key.base, key.alignment, key.natural_alignment,
                    access.residue, key.load, access.first, access.end });
            found = std::prev(windows_.end());
        }
        access.window = static_cast<unsigned>(found - windows_.begin());
        found->first = std::min(found->first, access.first);
        found->end = std::max(found->end, access.end);
        ++found->keys;
        found->unconditional |= access.unconditional;
        found->indexed |= key.index.has_value();
    }
    for (unsigned slot = 0; slot < slots; ++slot) {
        Window* best = nullptr;
        for (auto& w : windows_)
            if (!w.enabled && w.unconditional && (w.keys > 1 || w.indexed) &&
                fits(w.first, w.end) && (!best || w.keys > best->keys))
                best = &w;
        if (!best)
            break;
        best->enabled = true;
    }
}
std::optional<unsigned> AddressWindows::find(const Key& key) const
{
    const auto found = std::find_if(accesses_.begin(), accesses_.end(),
        [&](const Access& access) { return access.key == key; });
    if (found != accesses_.end() && found->window &&
        windows_[*found->window].enabled)
        return found->window;
    return { };
}
bool AddressWindows::empty() const
{
    return std::none_of(windows_.begin(), windows_.end(),
        [](const Window& w) { return w.enabled; });
}
CompiledTrace AddressWindows::select(
    CompiledTrace fast, CompiledTrace slow) const
{
    if (!fast.selectable_with(slow) ||
        fast.words.size() + slow.words.size() + 256 > 128U * 1024U / 4U)
        return slow;
    if (fast.words == slow.words)
        return fast;
    using namespace oaknut;
    using namespace oaknut::util;
    CompiledTrace result;
    static_cast<TraceMetadata&>(result) = fast;
    result.words.reserve(fast.words.size() + slow.words.size() + 256);
    VectorCodeGenerator code(result.words);
    Label fallback;
    for (const auto& w : windows_) {
        if (!w.enabled)
            continue;
        code.LDR(W16, X0, w.base * sizeof(std::uint32_t));
        code.AND(W16, W16, 4095);
        if (w.first < 0)
            code.SUB(W16, W16, static_cast<unsigned>(-w.first));
        else if (w.first != 0)
            code.ADD(W16, W16, static_cast<unsigned>(w.first));
        code.LSR(W17, W16, 12);
        code.CBNZ(W17, fallback);
        code.ADD(W16, W16, static_cast<unsigned>(w.end - w.first - 1));
        code.LSR(W17, W16, 12);
        code.CBNZ(W17, fallback);
    }
    code.align(16);
    result.words.insert(
        result.words.end(), fast.words.begin(), fast.words.end());
    code.align(16);
    code.l(fallback);
    result.words.insert(
        result.words.end(), slow.words.begin(), slow.words.end());
    return result;
}
}
