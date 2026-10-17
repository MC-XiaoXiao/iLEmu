/* SPDX-License-Identifier: MPL-2.0 */
#include "address_windows.hpp"
#include "arm/integer_semantics.hpp"
#include "register_writes.hpp"
#include "flag_effects.hpp"
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
    registers_.observe(inst, key && key->offset == 0 && !key->index && !key->pc_base);
    if (key && key->base < 15 && !key->pc_base) {
        auto found = std::find_if(accesses_.begin(), accesses_.end(),
            [&](const Access& access) { return access.key == *key; });
        if (found == accesses_.end()) {
            accesses_.push_back(Access { *key });
            found = std::prev(accesses_.end());
        }
        auto& access = *found;
        ++access.uses;
        std::int64_t low =
            key->add ? key->offset : -std::int64_t { key->offset };
        auto high = low;
        const auto displacement = displacements_.expression(key->base);
        const auto alignment = std::max(key->alignment, key->natural_alignment);
        bool valid = displacement.has_value();
        if (key->index) {
            const auto mask = bits_[*key->index];
            valid &= mask.has_value() && key->offset == 0;
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
        const auto constant = displacement ? displacement->constant : 0;
        const auto coefficient = displacement ? displacement->coefficient : 0;
        if (displacement && displacement->index) {
            valid &= (!access.stride || access.stride == displacement->index) &&
                !key->index && coefficient % static_cast<std::int32_t>(alignment) == 0;
            access.stride = displacement->index;
        }
        low += constant;
        high += std::int64_t { constant } + key->size;
        valid &= fits(low, high);
        const auto residue = static_cast<unsigned>(low) & (alignment - 1U);
        if ((access.end != 0 || access.first != 0) && access.residue != residue)
            valid = false;
        access.adjusted |= constant != 0 || coefficient != 0 || inst.writeback;
        if (access.valid && valid) {
            if (access.end == 0 && access.first == 0) {
                access.first = low;
                access.end = high;
                access.first_coefficient = access.last_coefficient = coefficient;
            } else {
                access.first = std::min(access.first, low);
                access.end = std::max(access.end, high);
                access.first_coefficient = std::min<std::int64_t>(
                    access.first_coefficient, coefficient);
                access.last_coefficient = std::max<std::int64_t>(
                    access.last_coefficient, coefficient);
            }
            access.residue = residue;
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
        if (counter_ == reg)
            counter_.reset();
    });
    if (flag_effects(inst).possible_write != 0)
        counter_.reset();
    if (inst.kind == arm::InstructionKind::DataProcessing &&
        inst.condition == 14 && inst.set_flags && inst.opcode == 2 &&
        inst.immediate_operand && inst.immediate == 1 &&
        inst.rd == inst.rn && inst.rd < 15)
        counter_ = inst.rd;
    if (result)
        bits_[inst.rd] = result;
    displacements_.observe(inst);
    if (inst.kind == arm::InstructionKind::Branch) {
        if (!counter_ || displacements_.get(*counter_) != -1 ||
            (inst.condition != 1 && inst.condition != 12))
            counter_.reset();
        signed_counter_ = inst.condition == 12;
    }
}
void AddressWindows::finish(unsigned slots, unsigned cycle_instructions)
{
    cycle_instructions_ = cycle_instructions;
    for (auto& access : accesses_) {
        const auto& key = access.key;
        const auto advance = displacements_.expression(key.base);
        const auto stride = advance && advance->index ? advance->index : access.stride;
        const auto alignment = static_cast<std::int32_t>(
            std::max(key.alignment, key.natural_alignment));
        if (!access.valid || !advance ||
            advance->constant % alignment != 0 || advance->coefficient % alignment != 0 ||
            (access.stride && advance->index && access.stride != advance->index) ||
            (stride && (!displacements_.invariant(*stride) || key.index)))
            continue;
        auto found = std::find_if(
            windows_.begin(), windows_.end(), [&](const Window& w) {
                return w.base == key.base && w.load == key.load &&
                       w.alignment == key.alignment &&
                       w.natural_alignment == key.natural_alignment &&
                       w.residue == access.residue && w.stride == stride;
            });
        if (found == windows_.end()) {
            windows_.push_back(
                Window { key.base, key.alignment, key.natural_alignment,
                    access.residue, key.load, access.first, access.end });
            found = std::prev(windows_.end());
            found->advance = advance->constant;
            found->stride = stride;
            found->stride_advance = advance->coefficient;
            found->first_coefficient = access.first_coefficient;
            found->last_coefficient = access.last_coefficient;
        }
        access.window = static_cast<unsigned>(found - windows_.begin());
        found->first = std::min(found->first, access.first);
        found->end = std::max(found->end, access.end);
        found->first_coefficient = std::min(found->first_coefficient,
            access.first_coefficient);
        found->last_coefficient = std::max(found->last_coefficient,
            access.last_coefficient);
        ++found->keys;
        found->uses += access.uses;
        found->indexed |= key.index.has_value();
        found->adjusted |= access.adjusted || displacements_.modified(key.base);
    }
    for (unsigned slot = 0; slot < slots; ++slot) {
        Window* best = nullptr;
        for (auto& w : windows_)
            if (!w.enabled && (w.keys > 1 || w.indexed || w.adjusted) &&
                fits(w.first, w.end) && (!best || w.keys > best->keys))
                best = &w;
        if (!best)
            break;
        best->enabled = true;
    }
}
void AddressWindows::expand(unsigned instructions)
{
    // A native group can contain several copies of the proven cycle. Bound
    // every copy before allowing its first resolved page pointer to be reused.
    if (cycle_instructions_ == 0 || instructions % cycle_instructions_ != 0) {
        for (auto& w : windows_)
            w.enabled = false;
        return;
    }
    const auto copies = instructions / cycle_instructions_;
    for (auto& w : windows_) {
        if (!w.enabled || (w.advance == 0 && w.stride_advance == 0))
            continue;
        if (copies == 0) {
            w.enabled = false;
            continue;
        }
        const auto last = std::int64_t { w.advance } * (copies - 1U);
        w.first += std::min<std::int64_t>(0, last);
        w.end += std::max<std::int64_t>(0, last);
        const auto stride_last = std::int64_t { w.stride_advance } * (copies - 1U);
        w.first_coefficient += std::min<std::int64_t>(0, stride_last);
        w.last_coefficient += std::max<std::int64_t>(0, stride_last);
        w.enabled = fits(w.first, w.end) &&
            w.first_coefficient >= INT32_MIN && w.last_coefficient <= INT32_MAX;
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
bool AddressWindows::advances() const
{
    return std::any_of(windows_.begin(), windows_.end(),
        [](const Window& w) {
            return w.enabled && (w.advance != 0 || w.stride_advance != 0);
        });
}
AddressRegisterPlan AddressWindows::register_plan() const
{
    AddressRegisterPlan result;
    const auto append = [&](bool deferred) {
        for (unsigned reg = 0; reg < 15 && result.count < result.capacity; ++reg) {
            if (!(deferred ? registers_.permits_constant_offsets(reg)
                          : registers_.permits(reg)))
                continue;
            std::optional<unsigned> selected;
            bool valid = true;
            for (const auto& access : accesses_) {
                if (access.key.base != reg)
                    continue;
                if (!access.window || !windows_[*access.window].enabled ||
                    windows_[*access.window].stride.has_value() == deferred ||
                    (selected && selected != access.window)) {
                    valid = false;
                    break;
                }
                selected = access.window;
            }
            if (valid && selected) {
                result.bases[result.count++] = reg;
                if (deferred)
                    result.deferred_mask |= 1U << reg;
            }
        }
    };
    // Preserve every existing pointer slot before adding constant-offset bases.
    append(false);
    append(true);
    return result;
}
void AddressWindows::guard_group(
    oaknut::VectorCodeGenerator& code, oaknut::Label& fallback) const
{
    guard(code, fallback, true);
}
void AddressWindows::guard(oaknut::VectorCodeGenerator& code,
    oaknut::Label& fallback, bool registers) const
{
    using namespace oaknut;
    using namespace oaknut::util;
    for (const auto& w : windows_) {
        if (!w.enabled || (registers && w.advance == 0 && w.stride_advance == 0))
            continue;
        if (w.stride) {
            // Entry runs before NativeFrame saves callee-saved registers;
            // group checks can use its established X21 scratch instead.
            const auto offset = registers ? X21 : X2;
            if (registers)
                code.MOV(WReg { offset.index() }, WReg { static_cast<int>(w.base) });
            else
                code.LDR(WReg { offset.index() }, X0, w.base * sizeof(std::uint32_t));
            code.AND(WReg { offset.index() }, WReg { offset.index() }, 4095);
            if (registers)
                code.MOV(W15, WReg { static_cast<int>(*w.stride) });
            else
                code.LDR(W15, X0, *w.stride * sizeof(std::uint32_t));
            const auto bound = [&](std::int64_t constant, std::int64_t coefficient) {
                // The unadjusted page offset is already within this page.
                if (constant == 0 && coefficient == 0)
                    return;
                if (coefficient != 0) {
                    code.MOV(W17, static_cast<std::uint32_t>(coefficient));
                    code.SMADDL(X16, W15, W17, offset);
                } else
                    code.MOV(X16, offset);
                if (constant < 0)
                    code.SUB(X16, X16, static_cast<unsigned>(-constant));
                else if (constant != 0)
                    code.ADD(X16, X16, static_cast<unsigned>(constant));
                // Signed products fit in64bits. A negative or out-of-page
                // endpoint has nonzero high bits; guest NZCV stays untouched.
                code.LSR(X17, X16, 12);
                code.CBNZ(X17, fallback);
            };
            if (w.first_coefficient == w.last_coefficient) {
                bound(w.first, w.first_coefficient);
                bound(w.end - 1, w.last_coefficient);
            } else {
                Label negative, done;
                code.TBNZ(W15, 31, negative);
                bound(w.first, w.first_coefficient);
                bound(w.end - 1, w.last_coefficient);
                code.B(done);
                code.l(negative);
                bound(w.first, w.last_coefficient);
                bound(w.end - 1, w.first_coefficient);
                code.l(done);
            }
            continue;
        }
        if (registers)
            code.MOV(W16, WReg { static_cast<int>(w.base) });
        else
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
}
unsigned AddressWindows::fallback_limit(unsigned instructions) const
{
    const auto copies = cycle_instructions_ ? instructions / cycle_instructions_ : 0U;
    if (copies < 4)
        return instructions;
    constexpr unsigned maximum_fallback_copies = 8;
    return cycle_instructions_ * std::min(maximum_fallback_copies, copies / 2U);
}
bool AddressWindows::can_select(const CompiledTrace& fast, const CompiledTrace& slow) const
{
    if (fast.maximum_ticks < slow.maximum_ticks || fast.instructions < slow.instructions ||
        fast.words.size() + slow.words.size() + 256 > 128U * 1024U / 4U ||
        fast.words == slow.words)
        return false;
    auto bounded = static_cast<const TraceMetadata&>(slow);
    if (!fast.selectable_with(bounded)) {
        if (!fast.closed || !slow.closed)
            return false;
        // Each body publishes its actual paid ticks. The cache may retain
        // the larger closed group's conservative budget and polling bound.
        bounded.maximum_ticks = fast.maximum_ticks;
        bounded.instructions = fast.instructions;
        if (!fast.selectable_with(bounded))
            return false;
    }
    const bool dynamic = std::any_of(windows_.begin(), windows_.end(),
        [](const Window& w) { return w.enabled && w.stride.has_value(); });
    const bool only_dynamic = std::all_of(windows_.begin(), windows_.end(),
        [](const Window& w) { return !w.enabled || w.stride.has_value(); });
    return !(dynamic && only_dynamic && fast.words.size() >= slow.words.size());
}
CompiledTrace AddressWindows::select(
    CompiledTrace fast, CompiledTrace slow, bool revisit) const
{
    if (fast.words == slow.words && fast.selectable_with(slow))
        return fast;
    if (!can_select(fast, slow))
        return slow;
    const bool dynamic = std::any_of(windows_.begin(), windows_.end(),
        [](const Window& w) { return w.enabled && w.stride.has_value(); });
    using namespace oaknut;
    using namespace oaknut::util;
    CompiledTrace result;
    static_cast<TraceMetadata&>(result) = fast;
    result.words.reserve(fast.words.size() + slow.words.size() + 256);
    VectorCodeGenerator code(result.words);
    Label fallback;
    // Two signed endpoint checks per stream need several reused iterations.
    // A proven decrement/conditional backedge identifies the remaining work.
    // Mixed static windows and within-cycle reuse keep their existing choice.
    if (counter_ && dynamic && std::all_of(windows_.begin(), windows_.end(),
            [](const Window& w) { return !w.enabled || (w.stride && w.uses == 1); })) {
        constexpr unsigned short_loop_iterations = 4;
        code.LDR(W16, X0, *counter_ * sizeof(std::uint32_t));
        code.CMP(W16, short_loop_iterations);
        code.B(signed_counter_ ? Cond::LE : Cond::LS, fallback);
        // Both paths enter NativeFrame, which installs guest NZCV anew.
    }
    guard(code, fallback, false);
    code.align(16);
    result.words.insert(
        result.words.end(), fast.words.begin(), fast.words.end());
    code.align(16);
    code.l(fallback);
    if (revisit && advances()) {
        // Revisit the selector after one guarded group. A group crossing a
        // page boundary must not keep later page-local groups on the slow body.
        code.MOV(X16, 1);
        code.STR(X16, X1, offsetof(NativeOutcome, groups));
        code.align(16);
    }
    result.words.insert(
        result.words.end(), slow.words.begin(), slow.words.end());
    return result;
}
}
