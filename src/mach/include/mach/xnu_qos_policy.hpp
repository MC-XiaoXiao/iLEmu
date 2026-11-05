/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace ilemu {

// XNU thread_policy.h: ordered internal tiers, independent of pthread encoding.
enum class XnuQosTier : std::uint32_t {
    Unspecified,
    Maintenance,
    Background,
    Utility,
    Default,
    UserInitiated,
    UserInteractive,
};

struct XnuThreadQosPolicy {
    XnuQosTier tier { };
    std::int32_t relative_priority { };
    bool operator==(const XnuThreadQosPolicy&) const = default;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return tier <= XnuQosTier::UserInteractive && relative_priority <= 0 &&
               relative_priority >= -15 &&
               (tier != XnuQosTier::Unspecified || relative_priority == 0);
    }
    [[nodiscard]] constexpr std::int32_t priority() const noexcept
    {
        // XNU thread_qos_policy_params and thread_qos_scaled_relative_priority.
        constexpr std::array<std::int32_t, 7> bases { 0, 4, 4, 20, 31, 37, 46 };
        const auto index = static_cast<std::uint32_t>(tier);
        const auto lower =
            tier == XnuQosTier::Background ? 0 : bases[index - (index != 0)];
        return bases[index] -
               ((bases[index] - lower) * -relative_priority >> 4);
    }
};

struct XnuTaskQosPolicy {
    XnuQosTier ceiling { };
    XnuQosTier clamp { };
    bool urgent_ui { };
    bool background { };
    bool operator==(const XnuTaskQosPolicy&) const = default;

    [[nodiscard]] constexpr std::int32_t effective_priority(
        XnuThreadQosPolicy requested, std::int32_t legacy,
        const std::int32_t* donation) const noexcept
    {
        auto tier = requested.tier;
        if (tier != XnuQosTier::Unspecified && donation) {
            // Donation callbacks carry the class base, without relative
            // priority.
            for (auto value = XnuQosTier::UserInteractive;
                value != XnuQosTier::Unspecified;
                value = static_cast<XnuQosTier>(
                    static_cast<std::uint32_t>(value) - 1U)) {
                if (XnuThreadQosPolicy { value }.priority() == *donation) {
                    tier = std::max(tier, value);
                    break;
                }
            }
        }
        if (clamp != XnuQosTier::Unspecified)
            tier =
                tier == XnuQosTier::Unspecified ? clamp : std::min(tier, clamp);
        if (ceiling != XnuQosTier::Unspecified &&
            tier != XnuQosTier::Unspecified)
            tier = std::min(tier, ceiling);
        // Native DARWIN_BG applies after task ceilings and also puts legacy
        // (unspecified-QoS) threads into the background class.
        if (background &&
            (tier == XnuQosTier::Unspecified || tier > XnuQosTier::Background))
            tier = XnuQosTier::Background;
        if (tier == XnuQosTier::Unspecified)
            return legacy;
        const auto relative = tier == requested.tier && !donation
                                  ? requested.relative_priority
                                  : 0;
        return XnuThreadQosPolicy { tier, relative }.priority() +
               (tier == XnuQosTier::UserInteractive && urgent_ui ? 1 : 0);
    }
};

} // namespace ilemu
