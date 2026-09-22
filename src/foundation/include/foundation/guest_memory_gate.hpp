// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <condition_variable>
#include <mutex>

namespace ilemu {

// Metadata transactions serialize checked users. Native leases are separate:
// a transaction drains only leases whose direct pointers it may invalidate.
class GuestMemoryGate {
public:
    using Conflict = bool (*)(const void* resource, const void* operation) noexcept;

    class Lease {
    public:
        using Interrupt = void (*)(void*) noexcept;
        Lease(GuestMemoryGate& gate, Interrupt interrupt, void* context,
            const void* resource)
            : gate_ { gate }, previous_ { current_ }, interrupt_ { interrupt },
              context_ { context }, resource_ { resource }
        {
            std::lock_guard lock { gate_.leases_mutex_ };
            blocked_ = gate_.transaction_active_;
            next_ = gate_.leases_;
            gate_.leases_ = this;
            current_ = this;
        }
        ~Lease()
        {
            std::lock_guard lock { gate_.leases_mutex_ };
            active_ = false;
            auto** entry = &gate_.leases_;
            while (*entry != this)
                entry = &(*entry)->next_;
            *entry = next_;
            current_ = previous_;
            gate_.leases_changed_.notify_all();
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        void suspend()
        {
            if (depth_++ == 0 && enabled_) {
                std::lock_guard lock { gate_.leases_mutex_ };
                active_ = false;
                gate_.leases_changed_.notify_all();
            }
        }
        void resume()
        {
            if (depth_ == 1 && enabled_) {
                std::unique_lock lock { gate_.leases_mutex_ };
                gate_.leases_changed_.wait(lock, [this] { return !blocked_; });
                active_ = true;
            }
            --depth_;
        }
        // Called while suspended by a metadata transaction.
        void disable() noexcept { enabled_ = false; }
        [[nodiscard]] bool held() const noexcept { return enabled_ && depth_ == 0; }

    private:
        friend class GuestMemoryGate;
        GuestMemoryGate& gate_;
        Lease* previous_;
        Lease* next_ { };
        Interrupt interrupt_;
        void* context_;
        const void* resource_;
        bool active_ { };
        bool blocked_ { };
        unsigned depth_ { 1 };
        bool enabled_ { true };
    };

    // Acquire metadata without touching native data or revoking any pointer.
    // The caller must classify the operation, then quiesce before data access
    // or mutation. Until then, existing native users run but cannot re-enter.
    void lock_metadata()
    {
        auto* lease = current_lease();
        if (lease)
            lease->suspend();
        try {
            mutex_.lock();
        } catch (...) {
            if (lease)
                lease->resume();
            throw;
        }
        std::lock_guard lock { leases_mutex_ };
        transaction_active_ = true;
        for (auto* reader = leases_; reader; reader = reader->next_)
            reader->blocked_ = true;
    }

    // Requires the metadata transaction. The predicate only reads stable view
    // metadata. Null means a full mapping/lifecycle boundary. No callback may
    // acquire another lock: interrupts merely request an atomic JIT halt.
    void quiesce(Conflict conflict = nullptr, const void* operation = nullptr)
    {
        std::unique_lock lock { leases_mutex_ };
        for (auto* reader = leases_; reader; reader = reader->next_) {
            reader->blocked_ = !conflict || conflict(reader->resource_, operation);
            if (reader->blocked_ && reader->active_)
                reader->interrupt_(reader->context_);
        }
        leases_changed_.notify_all();
        leases_changed_.wait(lock, [this] {
            for (auto* reader = leases_; reader; reader = reader->next_)
                if (reader->blocked_ && reader->active_)
                    return false;
            return true;
        });
    }

    void lock()
    {
        lock_metadata();
        try {
            quiesce();
        } catch (...) {
            unlock();
            throw;
        }
    }
    void unlock()
    {
        {
            std::lock_guard lock { leases_mutex_ };
            transaction_active_ = false;
            for (auto* reader = leases_; reader; reader = reader->next_)
                reader->blocked_ = false;
        }
        leases_changed_.notify_all();
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
    std::mutex mutex_;
    std::mutex leases_mutex_;
    std::condition_variable leases_changed_;
    Lease* leases_ { };
    bool transaction_active_ { };
};

} // namespace ilemu
