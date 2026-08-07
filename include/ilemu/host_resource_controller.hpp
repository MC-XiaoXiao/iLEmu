#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace ilemu {

enum class HostWorkKind : std::uint8_t {
  BackgroundCompile,
  OfflineCompile,
  Maintenance,
};

struct HostResourceBudget {
  std::size_t worker_count{1};
  std::chrono::nanoseconds duty_period{std::chrono::seconds{1}};
  std::chrono::nanoseconds interactive_compile_budget{
      std::chrono::milliseconds{100}};
  std::chrono::nanoseconds deadline_reserve{
      std::chrono::milliseconds{2}};
};

class HostWorkToken {
public:
  void cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
  [[nodiscard]] bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
  }

private:
  friend class HostResourceController;
  void mark_finished() noexcept {
    finished_.store(true, std::memory_order_release);
  }

  std::atomic<bool> cancelled_{};
  std::atomic<bool> finished_{};
};

class HostResourceController {
public:
  using Clock = std::chrono::steady_clock;
  using Work = std::function<void()>;

  explicit HostResourceController(HostResourceBudget budget = {});
  ~HostResourceController();

  HostResourceController(const HostResourceController &) = delete;
  HostResourceController &operator=(const HostResourceController &) = delete;

  // A deadline is advisory: work is rejected only when the controller knows
  // that the next deadline is inside the reserve window. The caller can retry
  // after the deadline advances.
  [[nodiscard]] std::shared_ptr<HostWorkToken> submit(
      HostWorkKind kind, std::optional<Clock::time_point> deadline,
      Work work);
  void set_next_deadline(std::optional<Clock::time_point> deadline);
  void wait_idle();

  [[nodiscard]] std::size_t queued() const;
  [[nodiscard]] std::size_t active() const;
  [[nodiscard]] std::uint64_t completed() const;
  [[nodiscard]] std::uint64_t rejected() const;

private:
  struct Task {
    HostWorkKind kind{};
    std::optional<Clock::time_point> deadline;
    std::uint64_t sequence{};
    std::shared_ptr<HostWorkToken> token;
    Work work;
  };

  [[nodiscard]] static bool task_precedes(const Task &left,
                                          const Task &right);
  void worker_loop();
  void stop();

  const HostResourceBudget budget_;
  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::map<std::uint64_t, Task> tasks_;
  std::vector<std::thread> workers_;
  std::optional<Clock::time_point> next_deadline_;
  Clock::time_point duty_window_start_{};
  std::chrono::nanoseconds interactive_work_{};
  std::uint64_t next_sequence_{1};
  std::uint64_t active_tasks_{};
  std::uint64_t completed_{};
  std::uint64_t rejected_{};
  bool stopping_{};
};

} // namespace ilemu
