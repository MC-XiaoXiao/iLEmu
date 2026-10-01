// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

// Notification ownership is distinct from the process's scheduler hold.
// All access is serialized by mach_mutex; no queue or allocation is needed.
class ChildWaitStatus {
public:
    void stopped(std::uint32_t signal) noexcept
    {
        stop_signal_ = static_cast<std::uint8_t>(signal);
        continued_ = false;
    }
    void continued() noexcept
    {
        stop_signal_ = 0;
        continued_ = true;
    }
    [[nodiscard]] std::optional<std::uint32_t> take(bool stopped,
        std::uint32_t options) noexcept
    {
        // P_LWAITED/P_WAITED and P_CONTINUED are consumed before copyout,
        // including when copyout fails. Exit status follows a different path.
        if (stopped && stop_signal_ != 0 && (options & 2U)) {
            const auto status = (static_cast<std::uint32_t>(stop_signal_) << 8U) | 0x7fU;
            stop_signal_ = 0;
            return status;
        }
        if (continued_ && (options & 0x10U)) {
            continued_ = false;
            return (19U << 8U) | 0x7fU; // Darwin W_STOPCODE(SIGCONT)
        }
        return std::nullopt;
    }

private:
    std::uint8_t stop_signal_ { };
    bool continued_ { };
};

} // namespace ilemu
