#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>

namespace ilemu {

struct DarwinPthreadRegistration {
    std::uint32_t thread_start { };
    std::uint32_t workqueue_thread_start { };
    std::uint32_t pthread_size { };
    std::uint32_t reserved { };
    std::uint32_t target_concurrency { };
    std::uint32_t dispatch_queue_offset { };
};

struct DarwinWorkqueueItem {
    std::uint32_t address { };
    std::uint32_t priority { };
    std::uint32_t affinity { };
    bool overcommit { };
};

struct DarwinWorkqueueWorker {
    std::uint32_t processor { };
    std::uint32_t port_name { };
    std::uint32_t allocation_base { };
    std::uint32_t allocation_size { };
    std::uint32_t pthread_address { };
    std::uint32_t stack_bottom { };
    std::uint32_t priority { };
    bool idle { };
};

// Per-process state installed by Darwin's bsdthread_register syscall. The
// compatibility kernel owns scheduling and memory; this object only retains
// the Guest ABI registration across fork and clears it across exec.
class DarwinPthreadRuntime {
public:
    static constexpr std::uint32_t maximum_pthread_size = 64U * 1024U;
    // Darwin 10's legacy ABI exposes high/default/low queues; the overcommit
    // bit is carried separately in the priority argument.
    static constexpr std::uint32_t workqueue_priority_count = 3U;
    static constexpr std::uint32_t workqueue_overcommit = 0x0001'0000U;
    static constexpr std::size_t maximum_workqueue_workers = 64U;
    static constexpr std::size_t maximum_workqueue_items_per_priority = 64U;

    [[nodiscard]] bool register_process(DarwinPthreadRegistration registration);
    [[nodiscard]] const std::optional<DarwinPthreadRegistration>&
    registration() const noexcept
    {
        return registration_;
    }
    [[nodiscard]] bool open_workqueue(std::uint32_t processor_count) noexcept
    {
        if (!registration_)
            return false;
        workqueue_open_ = true;
        target_concurrency_.fill(processor_count == 0 ? 1U : processor_count);
        return true;
    }
    [[nodiscard]] bool workqueue_open() const noexcept
    {
        return workqueue_open_;
    }

    [[nodiscard]] bool enqueue_workitem(
        DarwinWorkqueueItem item, bool front = false);
    [[nodiscard]] std::optional<DarwinWorkqueueItem> take_workitem();
    [[nodiscard]] bool remove_workitem(
        std::uint32_t address, std::uint32_t priority);
    [[nodiscard]] bool should_create_worker(
        std::uint32_t priority, bool overcommit) const noexcept;
    [[nodiscard]] bool add_worker(DarwinWorkqueueWorker worker);
    [[nodiscard]] std::optional<DarwinWorkqueueWorker> worker(
        std::uint32_t processor) const;
    [[nodiscard]] std::optional<DarwinWorkqueueWorker> idle_worker() const;
    void mark_worker_running(std::uint32_t processor, std::uint32_t priority);
    void park_worker(std::uint32_t processor);
    void remove_worker(std::uint32_t processor);
    [[nodiscard]] bool set_target_concurrency(
        std::uint32_t priority, std::uint32_t concurrency);

    void prepare_exec() noexcept
    {
        registration_.reset();
        reset_workqueue();
    }
    void inherit_from(const DarwinPthreadRuntime& parent, bool fork) noexcept
    {
        registration_ = fork ? parent.registration_ : std::nullopt;
        // XNU constructs a fresh workqueue for a child process.  The pthread
        // ABI registration survives fork, but kernel workqueue state does not.
        reset_workqueue();
    }

private:
    void reset_workqueue() noexcept;

    std::optional<DarwinPthreadRegistration> registration_;
    bool workqueue_open_ { };
    std::array<std::deque<DarwinWorkqueueItem>, workqueue_priority_count>
        workitems_;
    std::map<std::uint32_t, DarwinWorkqueueWorker> workers_;
    std::array<std::uint32_t, workqueue_priority_count> target_concurrency_ { };
};

} // namespace ilemu
