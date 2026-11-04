/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "foundation/darwin_version.hpp"
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
};

} // namespace ilemu
