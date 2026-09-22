// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <atomic>
#include <mutex>
#include <shared_mutex>

namespace ilemu {

// Native execution holds a read lease. Checked accesses temporarily release
// their lease before taking the exclusive gate, so they never upgrade in place.
class GuestMemoryGate {
public:
    class Lease {
    public:
        using Interrupt = void (*)(void*) noexcept;
        Lease(GuestMemoryGate& gate, Interrupt interrupt, void* context)
            : gate_ { gate }, previous_ { current_ },
              interrupt_ { interrupt }, context_ { context }
        {
            std::lock_guard lock { gate_.leases_mutex_ };
            next_ = gate_.leases_;
            gate_.leases_ = this;
            current_ = this;
        }
        ~Lease()
        {
            active_.store(false, std::memory_order_relaxed);
            {
                std::lock_guard lock { gate_.leases_mutex_ };
                auto** entry = &gate_.leases_;
                while (*entry != this)
                    entry = &(*entry)->next_;
                *entry = next_;
            }
            if (enabled_ && depth_ == 0)
                gate_.mutex_.unlock_shared();
            current_ = previous_;
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        void suspend()
        {
            if (depth_++ == 0 && enabled_) {
                active_.store(false, std::memory_order_relaxed);
                gate_.mutex_.unlock_shared();
            }
        }
        void resume()
        {
            if (depth_ == 1 && enabled_) {
                for (;;) {
                    // Do not re-enter ahead of a writer that requested our
                    // pause. Also close the registration/acquisition race.
                    for (auto pending = gate_.pending_writers_.load(std::memory_order_acquire);
                         pending != 0;
                         pending = gate_.pending_writers_.load(std::memory_order_acquire))
                        gate_.pending_writers_.wait(pending, std::memory_order_acquire);
                    gate_.mutex_.lock_shared();
                    active_.store(true, std::memory_order_seq_cst);
                    if (gate_.pending_writers_.load(std::memory_order_seq_cst) == 0)
                        break;
                    active_.store(false, std::memory_order_relaxed);
                    gate_.mutex_.unlock_shared();
                }
            }
            --depth_;
        }
        // Called while the lease is suspended under its exclusive gate.
        void disable() noexcept { enabled_ = false; }
        [[nodiscard]] bool held() const noexcept { return enabled_ && depth_ == 0; }

    private:
        friend class GuestMemoryGate;
        GuestMemoryGate& gate_;
        Lease* previous_;
        Lease* next_ { };
        Interrupt interrupt_;
        void* context_;
        std::atomic<bool> active_ { false };
        unsigned depth_ { 1 };
        bool enabled_ { true };
    };

    void lock()
    {
        auto* lease = current_lease();
        if (lease)
            lease->suspend();
        try {
            if (!mutex_.try_lock()) {
                pending_writers_.fetch_add(1U, std::memory_order_seq_cst);
                try {
                    // Request a block-boundary exit rather than waiting for a
                    // full computation slice before a checked access can proceed.
                    // Registration protects callback lifetime; callbacks only
                    // publish an atomic JIT halt and never acquire memory locks.
                    {
                        std::lock_guard lock { leases_mutex_ };
                        for (auto* reader = leases_; reader; reader = reader->next_)
                            if (reader->active_.load(std::memory_order_seq_cst))
                                reader->interrupt_(reader->context_);
                    }
                    mutex_.lock();
                } catch (...) {
                    pending_writers_.fetch_sub(1U, std::memory_order_release);
                    pending_writers_.notify_all();
                    throw;
                }
                pending_writers_.fetch_sub(1U, std::memory_order_release);
                pending_writers_.notify_all();
            }
        } catch (...) {
            if (lease)
                lease->resume();
            throw;
        }
    }
    void unlock()
    {
        mutex_.unlock();
        if (auto* lease = current_lease())
            lease->resume();
    }

private:
    [[nodiscard]] Lease* current_lease() noexcept
    {
        for (auto* lease = current_; lease; lease = lease->previous_)
            if (&lease->gate_ == this)
                return lease;
        return nullptr;
    }
    inline static thread_local Lease* current_ { };
    std::shared_mutex mutex_;
    std::mutex leases_mutex_;
    Lease* leases_ { };
    std::atomic<unsigned> pending_writers_ { };
};

} // namespace ilemu
