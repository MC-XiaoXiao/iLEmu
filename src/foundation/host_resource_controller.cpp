#include "ilemu/host_resource_controller.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ilemu {

HostResourceController::HostResourceController(HostResourceBudget budget)
    : budget_{std::move(budget)}, duty_window_start_{Clock::now()} {
  if (budget_.duty_period <= std::chrono::nanoseconds::zero() ||
      budget_.interactive_compile_budget <
          std::chrono::nanoseconds::zero() ||
      budget_.interactive_compile_budget > budget_.duty_period ||
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
    HostWorkKind kind, std::optional<Clock::time_point> deadline, Work work) {
  if (!work) return nullptr;
  const auto token = std::make_shared<HostWorkToken>();
  const auto now = Clock::now();
  const std::lock_guard lock{mutex_};
  if (stopping_ || workers_.empty() ||
      (kind == HostWorkKind::BackgroundCompile && next_deadline_ &&
       *next_deadline_ <= now + budget_.deadline_reserve)) {
    ++rejected_;
    return nullptr;
  }
  tasks_.emplace(next_sequence_,
                 Task{kind, deadline, next_sequence_++, token, std::move(work)});
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
        }
        const auto iterator = tasks_.begin();
        const auto interactive =
            iterator->second.kind == HostWorkKind::BackgroundCompile;
        if (!stopping_ && interactive &&
            interactive_work_ >= budget_.interactive_compile_budget) {
          const auto wake_at = duty_window_start_ + budget_.duty_period;
          work_available_.wait_until(lock, wake_at);
          continue;
        }
        task = std::move(iterator->second);
        tasks_.erase(iterator);
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
      if (task.kind == HostWorkKind::BackgroundCompile) {
        interactive_work_ += elapsed;
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
