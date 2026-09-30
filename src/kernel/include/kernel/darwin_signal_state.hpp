// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "kernel/darwin_abi.hpp"
#include "network/darwin_abi_route.hpp"
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ilemu {

// XNU kern_sig.c: sigprocmask broadcasts; __pthread_sigmask changes only
// the calling uthread. Ordinary thread mask access does not allocate or scan.
class DarwinSignalState {
public:
    explicit DarwinSignalState(DarwinAbiEpoch epoch)
        : retain_retired_(epoch >= DarwinAbiEpoch::IphoneOs3)
        , wait_consumes_owner_(epoch >= DarwinAbiEpoch::IphoneOs2)
        , wait_reports_interrupt_(epoch >= DarwinAbiEpoch::IphoneOs3) { }
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
        threads_[processor] = { value & ~unmaskable, std::nullopt, true, 0, next_order_++ };
    }
    void retire(std::size_t processor)
    {
        if (processor < threads_.size()) {
            if (retain_retired_)
                retired_pending_ |= threads_[processor].pending & exec_mask;
            threads_[processor] = { };
        }
    }
    void reset(std::size_t processor, std::uint32_t value)
    {
        threads_.clear();
        retired_pending_ = 0;
        next_order_ = 1;
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
        static_cast<void>(update(processor, 3, value, Scope::Thread));
        threads_[processor].saved_mask = previous;
    }
    void resume(std::size_t processor)
    {
        if (processor < threads_.size()) {
            auto& thread = threads_[processor];
            thread.mask = thread.saved_mask.value_or(thread.mask);
            thread.saved_mask.reset();
            thread.wait_mask = 0;
        }
    }
    [[nodiscard]] std::uint32_t pending(std::size_t processor) const
    {
        return processor < threads_.size() ? threads_[processor].pending : 0U;
    }
    [[nodiscard]] std::uint32_t ready(std::size_t processor) const
    {
        return pending(processor) & (~mask(processor) | waiting_for(processor));
    }
    void begin_wait(std::size_t processor, std::uint32_t signals)
    {
        suspend(processor, ~signals);
        threads_[processor].wait_mask = signals;
    }
    [[nodiscard]] std::uint32_t waiting_for(std::size_t processor) const
    {
        return processor < threads_.size() ? threads_[processor].wait_mask : 0U;
    }
    void accept_wait_signal(std::size_t processor, std::uint32_t bit)
    {
        // psignal replaces uu_sigwait with the delivered bit until msleep returns.
        threads_[processor].wait_mask = bit;
        consume(processor, bit);
    }
    [[nodiscard]] bool wait_reports_interrupt() const { return wait_reports_interrupt_; }
    [[nodiscard]] std::optional<std::uint32_t> take_pending(
        std::size_t waiter, std::uint32_t signals)
    {
        std::optional<std::size_t> owner;
        for (std::size_t i = 0; i < threads_.size(); ++i)
            if ((threads_[i].pending & signals) &&
                (!owner || threads_[i].order < threads_[*owner].order))
                owner = i;
        if (!owner)
            return std::nullopt;
        const auto signal = static_cast<std::uint32_t>(
            std::countr_zero(threads_[*owner].pending & signals)) + 1U;
        // XNU 792 clears the caller's list; 1228+ clears the selected owner.
        consume(wait_consumes_owner_ ? *owner : waiter, 1U << (signal - 1U));
        return signal;
    }
    template <typename Eligible>
    [[nodiscard]] std::optional<std::size_t> select(std::uint32_t bit,
        Eligible eligible) const
    {
        std::optional<std::size_t> first, unblocked;
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            const auto& thread = threads_[i];
            if (!thread.active)
                continue;
            if (!first || thread.order < threads_[*first].order)
                first = i;
            if (eligible(i) && (!(thread.mask & bit) || (thread.wait_mask & bit)) &&
                (!unblocked || thread.order < threads_[*unblocked].order))
                unblocked = i;
        }
        return unblocked ? unblocked : first;
    }
    void queue(std::size_t processor, std::uint32_t bit)
    {
        if (processor >= threads_.size() || !threads_[processor].active)
            return;
        auto& list = threads_[processor].pending;
        if (bit & continue_mask)
            list &= ~stop_mask;
        if (bit & stop_mask)
            list &= ~continue_mask;
        list |= bit;
    }
    void consume(std::size_t processor, std::uint32_t bits)
    {
        if (processor < threads_.size())
            threads_[processor].pending &= ~bits;
    }
    void discard(std::uint32_t bits)
    {
        retired_pending_ &= ~bits;
        for (auto& thread : threads_)
            thread.pending &= ~bits;
    }
    template <typename Visitor>
    void for_each_ready(Visitor visit)
    {
        for (std::size_t i = 0; i < threads_.size(); ++i)
            if (ready(i))
                visit(i);
    }
    void exec(std::size_t processor)
    {
        auto list = pending(processor) | retired_pending_;
        if (retain_retired_)
            for (std::size_t i = 0; i < threads_.size(); ++i)
                if (i != processor)
                    list |= threads_[i].pending & exec_mask;
        const auto value = mask(processor);
        reset(processor, value);
        threads_[processor].pending = list;
    }
private:
    static constexpr std::uint32_t stop_mask =
        (1U << 16U) | (1U << 17U) | (1U << 20U) | (1U << 21U);
    static constexpr std::uint32_t continue_mask = 1U << 18U;
    // XNU signalvar.h execmask; uthread_cleanup transfers these in 1456+.
    static constexpr std::uint32_t exec_mask = stop_mask | continue_mask |
        (1U << 0U) | (1U << 1U) | (1U << 2U) | (1U << 8U) |
        (1U << 14U) | (1U << 29U) | (1U << 30U);
    const bool retain_retired_;
    const bool wait_consumes_owner_;
    const bool wait_reports_interrupt_;
    std::uint32_t retired_pending_ { };
    std::uint64_t next_order_ { 2 };
    struct Thread {
        std::uint32_t mask { };
        std::optional<std::uint32_t> saved_mask;
        bool active { };
        std::uint32_t pending { };
        std::uint64_t order { };
        std::uint32_t wait_mask { };
    };
    std::vector<Thread> threads_ { Thread { 0, std::nullopt, true, 0, 1 } };
};

} // namespace ilemu
