/* SPDX-License-Identifier: MPL-2.0 */
#include "kernel/pthread_qos_overrides.hpp"
#include <algorithm>

namespace ilemu {

bool DarwinPthreadQosOverride::start(
    std::optional<std::int32_t> priority) noexcept
{
    if (count_ == maximum_depth)
        return false;
    ++count_;
    priority_ = std::max(priority_, priority);
    return true;
}

bool DarwinPthreadQosOverride::end(bool retain_peak) noexcept
{
    if (count_ == 0U)
        return false;
    if (--count_ == 0U && !retain_peak)
        priority_.reset();
    return true;
}

void DarwinPthreadQosOverride::observe_priority(
    std::optional<std::int32_t> priority) noexcept
{
    priority_ = std::max(priority_, priority);
}

void DarwinPthreadQosOverride::clear_peak_if_balanced() noexcept
{
    if (count_ == 0U)
        priority_.reset();
}

void DarwinPthreadResourceQosOverrides::start(
    std::uint32_t resource, std::optional<std::int32_t> priority)
{
    auto& donation = resources_[resource];
    ++donation.count;
    donation.priority = std::max(donation.priority, priority);
}

void DarwinPthreadResourceQosOverrides::end(std::uint32_t resource)
{
    const auto found = resources_.find(resource);
    // XNU's resource removal is a no-op when no matching override exists.
    if (found != resources_.end() && --found->second.count == 0U)
        resources_.erase(found);
}

void DarwinPthreadResourceQosOverrides::boost_dispatch(
    std::optional<std::int32_t> priority) noexcept
{
    ++dispatch_count_;
    dispatch_priority_ = std::max(dispatch_priority_, priority);
}

void DarwinPthreadResourceQosOverrides::set_mutex(
    std::uint32_t resource, std::optional<std::int32_t> priority)
{
    if (priority)
        mutexes_.insert_or_assign(resource, *priority);
    else
        mutexes_.erase(resource);
}

std::optional<std::int32_t> DarwinPthreadResourceQosOverrides::priority() const noexcept
{
    auto result = std::max(dispatch_priority_, workqueue_priority_);
    for (const auto& [resource, donation] : resources_) {
        static_cast<void>(resource);
        result = std::max(result, donation.priority);
    }
    for (const auto& [resource, priority] : mutexes_) {
        static_cast<void>(resource);
        result = std::max(result, std::optional { priority });
    }
    return result;
}

} // namespace ilemu
