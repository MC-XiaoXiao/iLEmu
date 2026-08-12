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
  ArtifactCompaction,
};

struct HostResourceBudget {
  std::size_t worker_count{1};
  std::chrono::nanoseconds duty_period{std::chrono::seconds{1}};
  std::chrono::nanoseconds interactive_compile_budget{
      std::chrono::milliseconds{100}};
  std::chrono::nanoseconds offline_compile_budget{
      std::chrono::milliseconds{800}};
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
  using CancellableWork = std::function<void(const HostWorkToken &)>;

  explicit HostResourceController(HostResourceBudget budget = {});
  ~HostResourceController();

  HostResourceController(const HostResourceController &) = delete;
  HostResourceController &operator=(const HostResourceController &) = delete;

  // A deadline is advisory: work is rejected only when the controller knows
  // that the next deadline is inside the reserve window. The caller can retry
  // after the deadline advances. Background work reserves estimated_cost when
  // it starts; a zero estimate conservatively consumes the full configured
  // interactive compile budget for that duty window.
  [[nodiscard]] std::shared_ptr<HostWorkToken> submit(
      HostWorkKind kind, std::optional<Clock::time_point> deadline,
      Work work,
      std::chrono::nanoseconds estimated_cost =
          std::chrono::nanoseconds::zero());
  // The token is passed to the work body so a task that has already started
  // can stop at its own safe points. Cancellation remains cooperative: the
  // controller never interrupts a host function in the middle of an ABI or
  // filesystem operation.
  [[nodiscard]] std::shared_ptr<HostWorkToken> submit_cancellable(
      HostWorkKind kind, std::optional<Clock::time_point> deadline,
      CancellableWork work,
      std::chrono::nanoseconds estimated_cost =
          std::chrono::nanoseconds::zero());
  void set_next_deadline(std::optional<Clock::time_point> deadline);
  // Wakes workers after a queued token is cancelled so the cancellation can
  // be observed without waiting for the current duty window to expire.
  void wake() noexcept;
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
    std::chrono::nanoseconds estimated_cost{};
    bool budget_reserved{};
  };

  [[nodiscard]] static bool task_precedes(const Task &left,
                                          const Task &right);
  [[nodiscard]] std::shared_ptr<HostWorkToken> submit_impl(
      HostWorkKind kind, std::optional<Clock::time_point> deadline,
      Work work, CancellableWork cancellable_work,
      std::chrono::nanoseconds estimated_cost);
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
  std::chrono::nanoseconds interactive_reserved_{};
  std::chrono::nanoseconds offline_work_{};
  std::chrono::nanoseconds offline_reserved_{};
  std::uint64_t next_sequence_{1};
  std::uint64_t active_tasks_{};
  std::uint64_t completed_{};
  std::uint64_t rejected_{};
  bool stopping_{};
};

} // namespace ilemu
