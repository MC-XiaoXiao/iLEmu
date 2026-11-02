/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <atomic>
#include <cstdint>

namespace ilemu::execution {

// Host publishers only advance a generation. CPU callbacks consume it at
// existing cooperative checkpoints, without touching mutable Guest state.
class GuestCooperationSignal {
public:
    void notify() noexcept { generation_.fetch_add(1, std::memory_order_release); }
    [[nodiscard]] std::uint64_t generation() const noexcept
    {
        return generation_.load(std::memory_order_acquire);
    }
private:
    std::atomic<std::uint64_t> generation_ { };
};

class GuestCooperationScope {
public:
    GuestCooperationScope(const GuestCooperationSignal* signal,
        std::uint64_t observed) noexcept
        : previous_signal_ { signal_ }, previous_observed_ { observed_ }
    {
        signal_ = signal;
        observed_ = observed;
    }
    ~GuestCooperationScope()
    {
        signal_ = previous_signal_;
        observed_ = previous_observed_;
    }
    GuestCooperationScope(const GuestCooperationScope&) = delete;
    GuestCooperationScope& operator=(const GuestCooperationScope&) = delete;
    [[nodiscard]] static bool requested() noexcept
    {
        return signal_ && signal_->generation() != observed_;
    }
private:
    const GuestCooperationSignal* previous_signal_;
    std::uint64_t previous_observed_;
    inline static thread_local const GuestCooperationSignal* signal_ { };
    inline static thread_local std::uint64_t observed_ { };
};

} // namespace ilemu::execution
