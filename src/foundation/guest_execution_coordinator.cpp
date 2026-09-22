// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Run scheduler-selected guest CPU batches on coordinated host
// execution lanes.

#include "foundation/guest_execution_coordinator.hpp"

#include <atomic>
#include <condition_variable>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ilemu {

namespace {
struct NativeBatch {
    explicit NativeBatch(std::ptrdiff_t count) : entry { count } {}
    std::latch entry;
    std::atomic<std::uint64_t> tick_budget {
        std::numeric_limits<std::uint64_t>::max() };
};

class NativeEntry {
public:
    explicit NativeEntry(NativeBatch* barrier)
        : barrier_ { barrier }, previous_ { current_ }
    {
        current_ = this;
    }
    ~NativeEntry()
    {
        // Preparation can throw or return before reaching native execution.
        if (barrier_)
            barrier_->entry.count_down();
        current_ = previous_;
    }
    static bool available() noexcept
    {
        return current_ && current_->barrier_;
    }
    static std::uint64_t synchronize(std::uint64_t tick_budget)
    {
        if (!current_ || !current_->barrier_)
            return tick_budget;
        auto* barrier = current_->barrier_;
        current_->barrier_ = nullptr;
        auto common = barrier->tick_budget.load(std::memory_order_relaxed);
        while (tick_budget < common &&
               !barrier->tick_budget.compare_exchange_weak(
                   common, tick_budget, std::memory_order_relaxed)) {
        }
        barrier->entry.arrive_and_wait();
        return barrier->tick_budget.load(std::memory_order_relaxed);
    }
private:
    NativeBatch* barrier_;
    NativeEntry* previous_;
    inline static thread_local NativeEntry* current_ { };
};
} // namespace

bool GuestExecutionCoordinator::has_native_entry_barrier() noexcept
{
    return NativeEntry::available();
}

std::uint64_t GuestExecutionCoordinator::synchronize_native_entry(
    std::uint64_t tick_budget)
{
    return NativeEntry::synchronize(tick_budget);
}

struct GuestExecutionCoordinator::Impl {
    std::mutex mutex;
    std::condition_variable work_available;
    std::condition_variable batch_complete;
    std::vector<std::thread> workers;
    std::span<GuestExecutionRequest*> requests;
    std::size_t next_request { };
    std::size_t remaining { };
    std::uint64_t generation { };
    bool stopping { };
    NativeBatch* native_entry { };
};

GuestExecutionCoordinator::GuestExecutionCoordinator(std::size_t worker_count)
    : impl_ { std::make_unique<Impl>() }
{
    if (worker_count == 0U) {
        throw std::invalid_argument {
            "guest execution coordinator requires a worker"
        };
    }
    impl_->workers.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index)
            impl_->workers.emplace_back([this] { worker_loop(); });
    } catch (...) {
        {
            std::lock_guard lock { impl_->mutex };
            impl_->stopping = true;
        }
        impl_->work_available.notify_all();
        for (auto& worker : impl_->workers)
            worker.join();
        throw;
    }
}

GuestExecutionCoordinator::~GuestExecutionCoordinator()
{
    {
        std::lock_guard lock { impl_->mutex };
        impl_->stopping = true;
    }
    impl_->work_available.notify_all();
    for (auto& worker : impl_->workers)
        worker.join();
}

void GuestExecutionCoordinator::run(
    std::span<GuestExecutionRequest*> requests)
{
    if (requests.empty())
        return;
    for (auto* request : requests) {
        if (request == nullptr || request->cpu == nullptr) {
            throw std::invalid_argument {
                "guest execution request requires a CPU"
            };
        }
    }
    // Larger queues must remain runnable: waiting at entry would otherwise
    // consume every worker before all requests can arrive.
    bool distinct_slots = true;
    for (std::size_t i = 0; i < requests.size(); ++i)
        for (std::size_t j = 0; j < i; ++j)
            if (requests[i]->execution_slot == requests[j]->execution_slot ||
                requests[i]->cpu == requests[j]->cpu)
                distinct_slots = false;
    std::optional<NativeBatch> native_entry;
    if (distinct_slots && requests.size() <= impl_->workers.size())
        native_entry.emplace(
            static_cast<std::ptrdiff_t>(requests.size()));
    {
        std::lock_guard lock { impl_->mutex };
        if (impl_->remaining != 0U) {
            throw std::logic_error {
                "guest execution batches cannot overlap"
            };
        }
        impl_->native_entry = native_entry ? &*native_entry : nullptr;
        impl_->requests = requests;
        impl_->next_request = 0U;
        impl_->remaining = requests.size();
        if (++impl_->generation == 0U)
            ++impl_->generation;
    }
    impl_->work_available.notify_all();
    std::unique_lock lock { impl_->mutex };
    impl_->batch_complete.wait(lock, [this] {
        return impl_->remaining == 0U;
    });
    impl_->requests = { };
    impl_->native_entry = nullptr;
}

void GuestExecutionCoordinator::execute(GuestExecutionRequest& request) noexcept
{
    request.result = { };
    request.error = nullptr;
    try {
        request.result = request.single_step
                             ? request.cpu->step(request.execution_slot)
                             : request.cpu->run_cooperatively(
                                   request.tick_budget,
                                   request.host_slice_budget,
                                   request.execution_slot);
    } catch (...) {
        request.error = std::current_exception();
    }
}

void GuestExecutionCoordinator::worker_loop()
{
    std::uint64_t observed_generation { };
    std::unique_lock lock { impl_->mutex };
    for (;;) {
        impl_->work_available.wait(lock, [this, &observed_generation] {
            return impl_->stopping ||
                   impl_->generation != observed_generation;
        });
        if (impl_->stopping)
            return;
        observed_generation = impl_->generation;
        while (impl_->next_request < impl_->requests.size()) {
            auto* request = impl_->requests[impl_->next_request++];
            auto* native_entry = impl_->native_entry;
            lock.unlock();
            {
                NativeEntry entry { native_entry };
                execute(*request);
            }
            lock.lock();
            if (--impl_->remaining == 0U)
                impl_->batch_complete.notify_one();
        }
    }
}

} // namespace ilemu
