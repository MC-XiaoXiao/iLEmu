// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Guest packet-filter character device identities. The filter state and
// ioctl protocol belong to the emulated kernel, not the host filesystem.

#pragma once

#include <cstdint>
#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace ilemu::darwin::packet_filter {

inline constexpr std::string_view control_path = "/dev/pf";
inline constexpr std::string_view manager_path = "/dev/pfm";
inline constexpr std::string_view control_descriptor_kind = "packet-filter";
inline constexpr std::string_view manager_descriptor_kind =
    "packet-filter-manager";
inline constexpr std::uint32_t control_minor = 0;
inline constexpr std::uint32_t manager_minor = 1;
inline constexpr std::uint32_t ioctl_set_debug = 0xc0044418U;
inline constexpr std::uint32_t ioctl_get_timeout = 0xc008441eU;
inline constexpr std::uint32_t ioctl_set_timeout = 0xc008441dU;
inline constexpr std::uint32_t ioctl_get_limit = 0xc0084427U;
inline constexpr std::uint32_t ioctl_set_limit = 0xc0084428U;
inline constexpr std::uint32_t ioctl_set_interface_flag = 0xc0284459U;
inline constexpr std::uint32_t ioctl_clear_interface_flag = 0xc028445aU;

class State {
public:
    void set_debug(std::uint32_t level)
    {
        std::lock_guard lock { mutex_ };
        debug_level_ = level;
    }

    [[nodiscard]] std::optional<std::uint32_t> get_limit(
        std::uint32_t index) const
    {
        std::lock_guard lock { mutex_ };
        if (index >= limits_.size())
            return std::nullopt;
        return limits_[index];
    }

    [[nodiscard]] std::optional<std::uint32_t> set_limit(
        std::uint32_t index, std::uint32_t limit)
    {
        std::lock_guard lock { mutex_ };
        if (index >= limits_.size())
            return std::nullopt;
        const auto old_limit = limits_[index];
        limits_[index] = limit;
        return old_limit;
    }

    [[nodiscard]] std::optional<std::uint32_t> get_timeout(
        std::uint32_t index) const
    {
        std::lock_guard lock { mutex_ };
        if (index >= timeouts_.size())
            return std::nullopt;
        return timeouts_[index];
    }

    [[nodiscard]] std::optional<std::uint32_t> set_timeout(
        std::uint32_t index, std::uint32_t seconds)
    {
        std::lock_guard lock { mutex_ };
        if (index >= timeouts_.size() ||
            static_cast<std::int32_t>(seconds) < 0)
            return std::nullopt;
        const auto old_timeout = timeouts_[index];
        // PFTM_INTERVAL has a one-second minimum in XNU.
        timeouts_[index] = index == 21 && seconds == 0 ? 1 : seconds;
        return old_timeout;
    }

    void update_interface_flags(
        std::string_view filter, std::uint32_t flags, bool clear)
    {
        std::lock_guard lock { mutex_ };
        for (auto& [name, value] : interface_flags_) {
            if (!filter.empty() && filter != name) {
                const auto prefix_matches =
                    filter.size() < 16 &&
                    (filter.back() < '0' || filter.back() > '9') &&
                    name.starts_with(filter) && name.size() > filter.size() &&
                    name[filter.size()] >= '0' &&
                    name[filter.size()] <= '9';
                if (!prefix_matches)
                    continue;
            }
            if (clear)
                value &= ~flags;
            else
                value |= flags;
        }
    }

private:
    mutable std::mutex mutex_;
    std::uint32_t debug_level_ { 1 }; // PF_DEBUG_URGENT
    // XNU 2782 pf_pool_limits defaults, in PF_LIMIT_* index order.
    std::array<std::uint32_t, 6> limits_ {
        10000, 10000, 10000, 5000, 1000, 200000
    };
    // XNU 2782 pf_default_rule timeout values, in PFTM_* index order.
    std::array<std::uint32_t, 26> timeouts_ {
        120, 30, 86400, 900, 45, 90, 60, 30, 60, 20, 10, 120, 30,
        1800, 120, 30, 900, 60, 30, 60, 30, 10, 6000, 12000, 0, 30
    };
    std::map<std::string, std::uint32_t> interface_flags_ {
        { "ALL", 0 }, { "lo0", 0 }, { "en0", 0 }
    };
};

[[nodiscard]] inline std::optional<std::uint32_t> minor_for_path(
    std::string_view path)
{
    if (path == control_path)
        return control_minor;
    if (path == manager_path)
        return manager_minor;
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::uint32_t> minor_for_descriptor(
    std::string_view kind)
{
    if (kind == control_descriptor_kind)
        return control_minor;
    if (kind == manager_descriptor_kind)
        return manager_minor;
    return std::nullopt;
}

} // namespace ilemu::darwin::packet_filter
