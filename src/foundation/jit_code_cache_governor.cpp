// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Allocate native-code cache budgets and reclaim code according to
// runtime value.

#include "foundation/jit_code_cache_governor.hpp"
#include "jit_backend_capabilities.hpp"

#include <algorithm>
#include <limits>

namespace ilemu {
namespace {

    [[nodiscard]] std::size_t saturating_add(
        std::size_t left, std::size_t right) noexcept
    {
        return right > std::numeric_limits<std::size_t>::max() - left
                   ? std::numeric_limits<std::size_t>::max()
                   : left + right;
    }

    [[nodiscard]] std::size_t saturating_multiply(
        std::size_t left, std::size_t right) noexcept
    {
        return right != 0 && left > std::numeric_limits<std::size_t>::max() / right
            ? std::numeric_limits<std::size_t>::max() : left * right;
    }

} // namespace

std::size_t JitCodeCacheGovernor::recommended_total_budget(
    std::uint64_t effective_memory_bytes, bool effective_memory_known,
    std::uint64_t available_headroom_bytes,
    bool available_headroom_known) noexcept
{
    const auto effective =
        effective_memory_known
            ? effective_memory_bytes
            : static_cast<std::uint64_t>(maximum_adaptive_budget_bytes);
    const auto capacity_target = effective / 4U;
    const auto headroom_target = available_headroom_bytes / 2U;
    auto target = available_headroom_known
                      ? std::min(capacity_target, headroom_target)
                      : capacity_target;
    if (target == 0U)
        target = minimum_demand_working_set_bytes;
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(target,
        static_cast<std::uint64_t>(minimum_demand_working_set_bytes),
        static_cast<std::uint64_t>(maximum_adaptive_budget_bytes)));
}

JitCodeCacheReservation::JitCodeCacheReservation(JitCodeCacheGovernor& governor,
    std::size_t shared_slab_bytes, std::size_t mapped_bytes,
    JitCodeCacheClass cache_class) noexcept
    : governor_ { &governor }
    , shared_slab_bytes_ { shared_slab_bytes }
    , mapped_bytes_ { mapped_bytes }
    , cache_class_ { cache_class }
{
}

JitCodeCacheReservation::~JitCodeCacheReservation()
{
    if (governor_ != nullptr)
        governor_->release(mapped_bytes_, actual_bytes_);
}

JitCodeCacheGovernor::JitCodeCacheGovernor(
    std::size_t shared_slab_cap, std::size_t total_budget) noexcept
    : JitCodeCacheGovernor(shared_slab_cap, total_budget,
          { JitBackendCapabilities::shared_native_cache
                ? execution::NativeCacheLayout::Storage::Shared
                : execution::NativeCacheLayout::Storage::Local,
              JitBackendCapabilities::minimum_native_cache_bytes },
          minimum_demand_working_set_bytes)
{
}

JitCodeCacheGovernor::JitCodeCacheGovernor(std::size_t runtime_cap,
    std::size_t total_budget, execution::NativeCacheLayout layout) noexcept
    : JitCodeCacheGovernor(
          runtime_cap, total_budget, layout, layout.minimum_bytes)
{
}

JitCodeCacheGovernor::JitCodeCacheGovernor(std::size_t runtime_cap,
    std::size_t total_budget, execution::NativeCacheLayout layout,
    std::size_t working_set_floor) noexcept
    : layout_ { layout }
    , shared_slab_cap_ {
        layout.storage == execution::NativeCacheLayout::Storage::None
            ? 0U : std::max(runtime_cap, layout.minimum_bytes) }
    , total_budget_ { layout.storage == execution::NativeCacheLayout::Storage::None
              ? total_budget
              : std::max({ total_budget, working_set_floor, layout.minimum_bytes }) }
{
}

std::shared_ptr<JitCodeCacheReservation> JitCodeCacheGovernor::reserve(
    std::size_t processor_count, JitCodeCacheClass cache_class)
{
    if (processor_count == 0U)
        return { };
    std::lock_guard lock { mutex_ };
    // Shared backends map one slab; local backends divide the runtime capacity
    // across its execution slots instead of multiplying it by the CPU count.
    const auto mapping_count = layout_.mapping_count(processor_count);
    const auto emitter_bytes = mapping_count == 0 ? 0U
        : std::max(shared_slab_cap_ / mapping_count, layout_.minimum_bytes);
    const auto mapped_bytes = saturating_multiply(emitter_bytes, mapping_count);
    auto reservation = std::shared_ptr<JitCodeCacheReservation> {
        new JitCodeCacheReservation { *this, emitter_bytes, mapped_bytes, cache_class }
    };
    mapped_bytes_total_ = saturating_add(mapped_bytes_total_, mapped_bytes);
    return reservation;
}

std::optional<std::size_t> JitCodeCacheGovernor::reclassify(
    JitCodeCacheReservation& reservation, JitCodeCacheClass cache_class,
    bool slab_can_resize) noexcept
{
    std::lock_guard lock { mutex_ };
    static_cast<void>(slab_can_resize);
    reservation.cache_class_ = cache_class;
    // Mapping capacity is a backend/host property, not a lifecycle quota.
    // Returning it lets a not-yet-created emitter apply the same invariant.
    return reservation.shared_slab_bytes_;
}

void JitCodeCacheGovernor::refresh_actual(
    JitCodeCacheReservation& reservation, std::uint64_t actual_bytes) noexcept
{
    std::lock_guard lock { mutex_ };
    const auto previous_actual = reservation.actual_bytes_;
    reservation.actual_bytes_ = actual_bytes;
    if (actual_bytes > previous_actual) {
        const auto delta = actual_bytes - previous_actual;
        actual_bytes_total_ = delta > std::numeric_limits<std::size_t>::max()
                                  ? std::numeric_limits<std::size_t>::max()
                                  : saturating_add(actual_bytes_total_,
                                        static_cast<std::size_t>(delta));
    } else {
        const auto delta = previous_actual - actual_bytes;
        actual_bytes_total_ =
            delta > actual_bytes_total_
                ? 0U
                : actual_bytes_total_ - static_cast<std::size_t>(delta);
    }
}

void JitCodeCacheGovernor::set_pressure_limited(bool limited) noexcept
{
    std::lock_guard lock { mutex_ };
    pressure_limited_ = limited;
}

bool JitCodeCacheGovernor::pressure_limited() const noexcept
{
    std::lock_guard lock { mutex_ };
    return pressure_limited_;
}

std::size_t JitCodeCacheGovernor::shared_slab_cap() const noexcept
{
    return shared_slab_cap_;
}

std::size_t JitCodeCacheGovernor::reclaimable_bytes(
    const JitCodeCacheReservation& reservation) const noexcept
{
    std::lock_guard lock { mutex_ };
    if (!pressure_limited_ && actual_bytes_total_ <= total_budget_)
        return 0U;
    if (reservation.cache_class_ != JitCodeCacheClass::Background)
        return 0U;
    return static_cast<std::size_t>(reservation.actual_bytes_);
}

std::size_t JitCodeCacheGovernor::total_budget() const noexcept
{
    return total_budget_;
}

std::size_t JitCodeCacheGovernor::total_mapped() const noexcept
{
    std::lock_guard lock { mutex_ };
    return mapped_bytes_total_;
}

std::size_t JitCodeCacheGovernor::total_actual() const noexcept
{
    std::lock_guard lock { mutex_ };
    return actual_bytes_total_;
}

void JitCodeCacheGovernor::release(
    std::size_t mapped_bytes, std::uint64_t actual_bytes) noexcept
{
    std::lock_guard lock { mutex_ };
    mapped_bytes_total_ = mapped_bytes > mapped_bytes_total_
                              ? 0U
                              : mapped_bytes_total_ - mapped_bytes;
    actual_bytes_total_ =
        actual_bytes > actual_bytes_total_
            ? 0U
            : actual_bytes_total_ - static_cast<std::size_t>(actual_bytes);
}

} // namespace ilemu
