/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/darwin_version.hpp"
#include "mach/xnu_qos_policy.hpp"
#include <atomic>
#include <cstdint>
#include <optional>

namespace ilemu {

// XNU bsd/sys/resource.h: PRIO_DARWIN_ROLE is a task's UI role, not its nice value.
enum class DarwinProcessRole : std::int32_t {
    Default,
    FocalUi,
    Ui,
    NonUi,
    NonFocalUi,
    ThrottledLaunch,
};

enum class DarwinProcessType : std::uint32_t {
    Unspecified, App, TalApp, StandardDaemon, InteractiveDaemon,
    BackgroundDaemon, AdaptiveDaemon,
};

struct ProcessPriorityProfile {
    static constexpr std::uint32_t darwin_role_scope = 6U;
    bool darwin_roles { };
    std::int32_t maximum_role { };

    static constexpr ProcessPriorityProfile for_version(DarwinVersion version)
    {
        return { version >= DarwinGeneration::Ios8,
            version >= DarwinGeneration::Ios9 ? 5 : 3 };
    }
};

// Shared by a task's kernel and process-table record so remote policy requests
// and runtime scheduling observe the same state. A new task starts unspecified.
class ProcessExecutionPolicy {
public:
    [[nodiscard]] DarwinProcessRole role() const noexcept
    {
        return role_.load(std::memory_order_relaxed);
    }
    void set_role(DarwinProcessRole role) noexcept
    {
        role_.store(role, std::memory_order_relaxed);
    }
    void set_spawn_policy(DarwinProcessType type, XnuQosTier clamp) noexcept
    {
        if (type != DarwinProcessType::Unspecified)
            type_.store(type, std::memory_order_relaxed);
        if (clamp != XnuQosTier::Unspecified)
            clamp_.store(clamp, std::memory_order_relaxed);
    }
    [[nodiscard]] DarwinProcessType type() const noexcept
    {
        return type_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] XnuTaskQosPolicy qos_policy() const noexcept
    {
        XnuTaskQosPolicy result { XnuQosTier::UserInitiated,
            clamp_.load(std::memory_order_relaxed) };
        if (type() == DarwinProcessType::App || type() == DarwinProcessType::TalApp) {
            switch (role()) {
            case DarwinProcessRole::FocalUi:
                result.urgent_ui = true;
                [[fallthrough]];
            case DarwinProcessRole::Ui:
            case DarwinProcessRole::NonFocalUi:
                result.ceiling = XnuQosTier::Unspecified;
                break;
            case DarwinProcessRole::ThrottledLaunch:
                result.ceiling = XnuQosTier::Utility;
                break;
            default:
                result.ceiling = XnuQosTier::Default;
                break;
            }
        }
        return result;
    }
    [[nodiscard]] XnuThreadQosPolicy main_thread_qos() const noexcept
    {
        const auto process_type = type();
        auto tier = process_type == DarwinProcessType::App || process_type == DarwinProcessType::TalApp
            ? XnuQosTier::UserInteractive
            : process_type == DarwinProcessType::BackgroundDaemon
                ? XnuQosTier::Background : XnuQosTier::Default;
        const auto clamp = clamp_.load(std::memory_order_relaxed);
        if (clamp != XnuQosTier::Unspecified)
            tier = std::min(tier, clamp);
        return { tier };
    }
    [[nodiscard]] std::optional<bool> visible() const noexcept
    {
        const auto value = role();
        if (value == DarwinProcessRole::Default)
            return std::nullopt;
        return value != DarwinProcessRole::NonUi &&
               value != DarwinProcessRole::ThrottledLaunch;
    }

private:
    std::atomic<DarwinProcessRole> role_ { DarwinProcessRole::Default };
    std::atomic<DarwinProcessType> type_ { DarwinProcessType::Unspecified };
    std::atomic<XnuQosTier> clamp_ { XnuQosTier::Unspecified };
};

} // namespace ilemu
