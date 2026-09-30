// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "kernel/darwin_abi.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ilemu {

// XNU kern_sig.c: sigprocmask broadcasts; __pthread_sigmask changes only
// the calling uthread. Ordinary thread mask access does not allocate or scan.
class DarwinSignalMasks {
public:
    enum class Scope { Thread, Process };
    static constexpr std::uint32_t unmaskable =
        (1U << (darwin::signal::kill - 1U)) |
        (1U << (darwin::signal::stop - 1U));
    // XNU signal.h: ~(threadmask | sigcantmask). Synchronous
    // SIGILL/TRAP/IOT/EMT/FPE/BUS/SEGV/SYS/PIPE remain unblocked in workers.
    static constexpr std::uint32_t workqueue_mask =
        ~(unmaskable | (1U << 3U) | (1U << 4U) | (1U << 5U) |
            (1U << 6U) | (1U << 7U) | (1U << 9U) | (1U << 10U) |
            (1U << 11U) | (1U << 12U));

    [[nodiscard]] std::uint32_t mask(std::size_t processor) const
    {
        return processor < threads_.size() ? threads_[processor].mask : 0U;
    }
    [[nodiscard]] std::uint32_t inherited_mask(std::size_t processor) const
    {
        if (processor >= threads_.size())
            return 0;
        const auto& thread = threads_[processor];
        return thread.saved_mask.value_or(thread.mask);
    }
    void initialize(std::size_t processor, std::uint32_t value)
    {
        if (processor >= threads_.size())
            threads_.resize(processor + 1U);
        threads_[processor] = { value & ~unmaskable, std::nullopt, true };
    }
    void retire(std::size_t processor)
    {
        if (processor < threads_.size())
            threads_[processor] = { };
    }
    void reset(std::size_t processor, std::uint32_t value)
    {
        threads_.clear();
        initialize(processor, value);
    }
    [[nodiscard]] bool update(std::size_t processor, std::uint32_t how,
        std::uint32_t value, Scope scope)
    {
        if (how < 1U || how > 3U)
            return false;
        const auto apply = [how, value](Thread& thread) {
            switch (how) {
            case 1: thread.mask |= value; break;
            case 2: thread.mask &= ~value; break;
            case 3: thread.mask = value; break;
            }
            thread.mask &= ~unmaskable;
        };
        if (processor >= threads_.size() || !threads_[processor].active)
            initialize(processor, 0);
        if (scope == Scope::Thread) {
            apply(threads_[processor]);
        } else {
            for (auto& thread : threads_)
                if (thread.active)
                    apply(thread);
        }
        return true;
    }
    void suspend(std::size_t processor, std::uint32_t value)
    {
        const auto previous = mask(processor);
        initialize(processor, value);
        threads_[processor].saved_mask = previous;
    }
    void resume(std::size_t processor)
    {
        if (processor < threads_.size()) {
            auto& thread = threads_[processor];
            thread.mask = thread.saved_mask.value_or(thread.mask);
            thread.saved_mask.reset();
        }
    }
    [[nodiscard]] bool all_blocked(std::uint32_t bit) const
    {
        for (const auto& thread : threads_)
            if (thread.active && (thread.mask & bit) == 0)
                return false;
        return true;
    }
private:
    struct Thread {
        std::uint32_t mask { };
        std::optional<std::uint32_t> saved_mask;
        bool active { };
    };
    std::vector<Thread> threads_ { Thread { 0, std::nullopt, true } };
};

} // namespace ilemu
