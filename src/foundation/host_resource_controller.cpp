#include "ilemu/host_resource_controller.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {

[[nodiscard]] unsigned work_priority(HostWorkKind kind) {
  switch (kind) {
  case HostWorkKind::Maintenance:
    return 3;
  case HostWorkKind::BackgroundCompile:
    return 1;
  case HostWorkKind::OfflineCompile:
    return 2;
  }
  return 2;
}

[[nodiscard]] bool is_deferred_work(HostWorkKind kind) {
  return kind == HostWorkKind::BackgroundCompile ||
         kind == HostWorkKind::OfflineCompile ||
         kind == HostWorkKind::Maintenance;
}

[[nodiscard]] bool is_interactive_work(HostWorkKind kind) {
  return kind == HostWorkKind::BackgroundCompile;
}

} // namespace

bool HostResourceController::task_precedes(const Task &left,
                                           const Task &right) {
  if (left.deadline && right.deadline && *left.deadline != *right.deadline)
    return *left.deadline < *right.deadline;
  if (left.deadline != right.deadline)
    return left.deadline.has_value();
  const auto left_priority = work_priority(left.kind);
  const auto right_priority = work_priority(right.kind);
  if (left_priority != right_priority)
    return left_priority < right_priority;
  return left.sequence < right.sequence;
}

HostResourceController::HostResourceController(HostResourceBudget budget)
    : budget_{std::move(budget)}, duty_window_start_{Clock::now()} {
  if (budget_.duty_period <= std::chrono::nanoseconds::zero() ||
      budget_.interactive_compile_budget <
          std::chrono::nanoseconds::zero() ||
      budget_.interactive_compile_budget > budget_.duty_period ||
      budget_.offline_compile_budget < std::chrono::nanoseconds::zero() ||
      budget_.deadline_reserve < std::chrono::nanoseconds::zero()) {
    throw std::invalid_argument{"invalid host resource budget"};
  }
  workers_.reserve(budget_.worker_count);
  for (std::size_t index = 0; index < budget_.worker_count; ++index) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

HostResourceController::~HostResourceController() { stop(); }

std::shared_ptr<HostWorkToken> HostResourceController::submit(
    HostWorkKind kind, std::optional<Clock::time_point> deadline, Work work,
    std::chrono::nanoseconds estimated_cost) {
  if (!work) return nullptr;
  const auto token = std::make_shared<HostWorkToken>();
  const auto now = Clock::now();
  const std::lock_guard lock{mutex_};
  if (stopping_ || workers_.empty() ||
      (is_deferred_work(kind) && next_deadline_ &&
       *next_deadline_ <= now + budget_.deadline_reserve)) {
    ++rejected_;
    return nullptr;
  }
  if (is_deferred_work(kind)) {
    const auto limit = is_interactive_work(kind)
                           ? budget_.interactive_compile_budget
                           : budget_.offline_compile_budget;
    if (estimated_cost < std::chrono::nanoseconds::zero()) {
      ++rejected_;
      return nullptr;
    }
    if (estimated_cost == std::chrono::nanoseconds::zero())
      estimated_cost = limit;
    if (limit == std::chrono::nanoseconds::zero() ||
        estimated_cost > limit) {
      ++rejected_;
      return nullptr;
    }
  } else {
    estimated_cost = std::chrono::nanoseconds::zero();
  }
  const auto sequence = next_sequence_++;
  tasks_.emplace(sequence,
                 Task{kind, deadline, sequence, token, std::move(work),
                      estimated_cost});
  work_available_.notify_one();
  return token;
}

void HostResourceController::set_next_deadline(
    std::optional<Clock::time_point> deadline) {
  {
    const std::lock_guard lock{mutex_};
    next_deadline_ = deadline;
  }
  work_available_.notify_all();
}

void HostResourceController::wake() noexcept { work_available_.notify_all(); }

void HostResourceController::wait_idle() {
  std::unique_lock lock{mutex_};
  idle_.wait(lock, [this] {
    return tasks_.empty() && active_tasks_ == 0U;
  });
}

std::size_t HostResourceController::queued() const {
  const std::lock_guard lock{mutex_};
  return tasks_.size();
}

std::size_t HostResourceController::active() const {
  const std::lock_guard lock{mutex_};
  return static_cast<std::size_t>(active_tasks_);
}

std::uint64_t HostResourceController::completed() const {
  const std::lock_guard lock{mutex_};
  return completed_;
}

std::uint64_t HostResourceController::rejected() const {
  const std::lock_guard lock{mutex_};
  return rejected_;
}

void HostResourceController::worker_loop() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock{mutex_};
      for (;;) {
        work_available_.wait(lock, [this] {
          return stopping_ || !tasks_.empty();
        });
        if (tasks_.empty() && stopping_) return;
        if (tasks_.empty()) continue;

        const auto now = Clock::now();
        if (now - duty_window_start_ >= budget_.duty_period) {
          duty_window_start_ = now;
          interactive_work_ = std::chrono::nanoseconds::zero();
          offline_work_ = std::chrono::nanoseconds::zero();
        }
        const auto background_deadline_too_close =
            next_deadline_ &&
            *next_deadline_ <= now + budget_.deadline_reserve;
        auto iterator = tasks_.end();
        for (auto candidate = tasks_.begin(); candidate != tasks_.end();
             ++candidate) {
          if (candidate->second.token->cancelled()) {
            iterator = candidate;
            break;
          }
          const auto budgeted = is_deferred_work(candidate->second.kind);
          if (!stopping_ && budgeted) {
            if (background_deadline_too_close)
              continue;
            const auto interactive =
                is_interactive_work(candidate->second.kind);
            const auto limit = interactive
                                   ? budget_.interactive_compile_budget
                                   : budget_.offline_compile_budget;
            const auto work = interactive ? interactive_work_ : offline_work_;
            const auto reserved = interactive ? interactive_reserved_
                                              : offline_reserved_;
            if (work >= limit)
              continue;
            const auto available = limit - work;
            if (reserved > available ||
                candidate->second.estimated_cost >
                    available - reserved) {
              continue;
            }
          }
          if (iterator == tasks_.end() ||
              task_precedes(candidate->second, iterator->second)) {
            iterator = candidate;
          }
        }
        if (iterator == tasks_.end()) {
          const auto wake_at = duty_window_start_ + budget_.duty_period;
          work_available_.wait_until(lock, wake_at);
          continue;
        }
        task = std::move(iterator->second);
        tasks_.erase(iterator);
        if (is_interactive_work(task.kind)) {
          interactive_reserved_ += task.estimated_cost;
          task.budget_reserved = true;
        } else if (is_deferred_work(task.kind)) {
          offline_reserved_ += task.estimated_cost;
          task.budget_reserved = true;
        }
        ++active_tasks_;
        break;
      }
    }

    const auto started = Clock::now();
    if (!task.token->cancelled()) {
      try {
        task.work();
      } catch (...) {
        // Host maintenance and optional compilation are isolated from guest
        // execution. A failed task is observable through its absence, not an
        // exception escaping the worker thread.
      }
    }
    const auto elapsed = Clock::now() - started;
    {
      const std::lock_guard lock{mutex_};
      task.token->mark_finished();
      if (task.budget_reserved) {
        if (is_interactive_work(task.kind)) {
          interactive_reserved_ -= task.estimated_cost;
          interactive_work_ += elapsed;
        } else if (is_deferred_work(task.kind)) {
          offline_reserved_ -= task.estimated_cost;
          offline_work_ += elapsed;
        }
      }
      --active_tasks_;
      ++completed_;
      if (tasks_.empty() && active_tasks_ == 0U) idle_.notify_all();
    }
    work_available_.notify_all();
  }
}

void HostResourceController::stop() {
  {
    const std::lock_guard lock{mutex_};
    if (stopping_) return;
    stopping_ = true;
    for (auto &[sequence, task] : tasks_) {
      static_cast<void>(sequence);
      task.token->cancel();
    }
  }
  work_available_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
}

} // namespace ilemu
