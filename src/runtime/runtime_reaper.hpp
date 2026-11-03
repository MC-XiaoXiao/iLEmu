// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "process.hpp"

namespace ilemu::runtime_detail {

// Runtime owners already occupy memory in the session. Moving them here adds
// no resource lifetime; it removes dead tasks from coordinator scans promptly.
// Repeated exec can create additional old mappings, so those retain their own
// two-owner limit, including the one being destroyed.
class RuntimeReaper {
public:
    struct Statistics {
        std::size_t runtimes {};
        std::size_t mappings {};
        std::size_t synchronous_clears {};
        std::size_t peak_runtimes {};
        std::uint64_t drain_nanoseconds {};
    };

    RuntimeReaper() : worker_ { [this] { worker_loop(); } } {}
    RuntimeReaper(const RuntimeReaper&) = delete;
    RuntimeReaper& operator=(const RuntimeReaper&) = delete;
    ~RuntimeReaper() { finish(); }

    void retire(std::unique_ptr<Runtime> runtime)
    {
        {
            std::lock_guard lock { mutex_ };
            runtimes_.push_back(std::move(runtime));
            ++statistics_.runtimes;
            statistics_.peak_runtimes =
                std::max(statistics_.peak_runtimes, runtimes_.size());
        }
        work_available_.notify_one();
    }

    void clear_address_space(AddressSpace& memory)
    {
        {
            std::lock_guard lock { mutex_ };
            if (mapping_count_ < 2U) {
                mappings_.push_back(memory.retire_mappings());
                ++mapping_count_;
                ++statistics_.mappings;
                work_available_.notify_one();
                return;
            }
        }
        ++statistics_.synchronous_clears;
        memory.clear();
    }

    void finish()
    {
        if (!worker_.joinable())
            return;
        const auto started = std::chrono::steady_clock::now();
        {
            std::lock_guard lock { mutex_ };
            stopping_ = true;
        }
        work_available_.notify_one();
        worker_.join();
        statistics_.drain_nanoseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
    }

    // Read after finish(), when all resources and worker references are gone.
    [[nodiscard]] const Statistics& statistics() const { return statistics_; }

private:
    void worker_loop()
    {
        std::unique_lock lock { mutex_ };
        for (;;) {
            work_available_.wait(lock, [this] {
                return stopping_ || !mappings_.empty() || !runtimes_.empty();
            });
            // Short, detached exec mappings must not wait behind every queued
            // Runtime's native metadata. Their lifetime is independent now
            // that CPU pools are never detached from their owning Runtime.
            if (!mappings_.empty() && (mapping_turn_ || runtimes_.empty())) {
                auto mappings = std::move(mappings_.front());
                mappings_.pop_front();
                lock.unlock();
                mappings.reset();
                lock.lock();
                --mapping_count_;
                mapping_turn_ = false;
            } else if (!runtimes_.empty()) {
                auto runtime = std::move(runtimes_.front());
                runtimes_.pop_front();
                lock.unlock();
                runtime.reset();
                lock.lock();
                mapping_turn_ = true;
            } else if (stopping_) {
                return;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable work_available_;
    std::deque<std::unique_ptr<Runtime>> runtimes_;
    std::deque<std::shared_ptr<AddressSpace::RetiredMappings>> mappings_;
    std::size_t mapping_count_ {};
    bool mapping_turn_ { true };
    bool stopping_ {};
    Statistics statistics_;
    std::thread worker_;
};

} // namespace ilemu::runtime_detail
