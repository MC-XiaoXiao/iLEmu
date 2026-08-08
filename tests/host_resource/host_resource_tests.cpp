#include "ilemu/host_resource_controller.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "ilemu/deadline_queue.hpp"
#include "ilemu/performance.hpp"

int main() {
  using namespace std::chrono_literals;
  ilemu::DeadlineQueue<int, std::uint64_t> deadlines;
  deadlines.upsert(1, 30U);
  deadlines.upsert(2, 10U);
  deadlines.upsert(1, 5U);
  if (deadlines.size() != 2U || deadlines.next_deadline() !=
                                   std::optional<std::uint64_t>{5U} ||
      !deadlines.erase(1) ||
      deadlines.next_deadline() != std::optional<std::uint64_t>{10U} ||
      deadlines.erase(1)) {
    std::cerr << "deadline queue did not maintain indexed earliest event\n";
    return 1;
  }
  deadlines.clear();
  if (deadlines.next_deadline()) {
    std::cerr << "deadline queue clear left a stale event\n";
    return 1;
  }

  ilemu::HostResourceBudget budget;
  budget.worker_count = 1U;
  budget.interactive_compile_budget = 100ms;
  budget.deadline_reserve = 2ms;
  ilemu::HostResourceController controller{budget};

  std::atomic<bool> background_ran{};
  std::atomic<bool> maintenance_ran{};
  const auto caller = std::this_thread::get_id();
  std::atomic<bool> worker_is_distinct{};
  const auto background = controller.submit(
      ilemu::HostWorkKind::BackgroundCompile, std::nullopt,
      [&] {
        background_ran.store(true, std::memory_order_release);
        worker_is_distinct.store(std::this_thread::get_id() != caller,
                                 std::memory_order_release);
      },
      1ms);
  const auto maintenance = controller.submit(
      ilemu::HostWorkKind::Maintenance, std::nullopt,
      [&] { maintenance_ran.store(true, std::memory_order_release); });
  if (!background || !maintenance) {
    std::cerr << "host work was not accepted\n";
    return 1;
  }
  controller.wait_idle();
  if (!background_ran.load(std::memory_order_acquire) ||
      !maintenance_ran.load(std::memory_order_acquire) ||
      !worker_is_distinct.load(std::memory_order_acquire) ||
      controller.completed() != 2U) {
    std::cerr << "host work did not run on the worker\n";
    return 1;
  }

  controller.set_next_deadline(
      ilemu::HostResourceController::Clock::now() + 1ms);
  if (controller.submit(ilemu::HostWorkKind::BackgroundCompile, std::nullopt,
                        [] {}, 1ms) != nullptr ||
      controller.rejected() == 0U) {
    std::cerr << "near-deadline compile work was not rejected\n";
    return 1;
  }
  controller.set_next_deadline(std::nullopt);
  const auto retry = controller.submit(
      ilemu::HostWorkKind::BackgroundCompile, std::nullopt, [] {}, 1ms);
  if (!retry) {
    std::cerr << "compile work was not accepted after the deadline moved\n";
    return 1;
  }
  retry->cancel();
  controller.wait_idle();

  std::mutex ordering_mutex;
  std::condition_variable ordering_condition;
  bool blocker_started = false;
  bool release_blocker = false;
  std::vector<int> ordering;
  const auto blocker = controller.submit(
      ilemu::HostWorkKind::Maintenance, std::nullopt, [&] {
        std::unique_lock lock{ordering_mutex};
        blocker_started = true;
        ordering_condition.notify_all();
        ordering_condition.wait(lock, [&] { return release_blocker; });
        ordering.push_back(0);
      });
  if (!blocker) {
    std::cerr << "ordering blocker was not accepted\n";
    return 1;
  }
  {
    std::unique_lock lock{ordering_mutex};
    ordering_condition.wait(lock, [&] { return blocker_started; });
  }
  const auto now = ilemu::HostResourceController::Clock::now();
  const auto early = controller.submit(
      ilemu::HostWorkKind::BackgroundCompile, now + 2ms,
      [&] {
        std::lock_guard lock{ordering_mutex};
        ordering.push_back(1);
      },
      1ms);
  const auto late = controller.submit(
      ilemu::HostWorkKind::OfflineCompile, now + 20ms,
      [&] {
        std::lock_guard lock{ordering_mutex};
        ordering.push_back(2);
      });
  const auto unbound = controller.submit(
      ilemu::HostWorkKind::Maintenance, std::nullopt,
      [&] {
        std::lock_guard lock{ordering_mutex};
        ordering.push_back(3);
      });
  if (!early || !late || !unbound) {
    std::cerr << "ordering tasks were not accepted\n";
    return 1;
  }
  {
    std::lock_guard lock{ordering_mutex};
    release_blocker = true;
  }
  ordering_condition.notify_all();
  controller.wait_idle();
  if (ordering != std::vector<int>{0, 1, 2, 3}) {
    std::cerr << "host work did not honor deadline ordering\n";
    return 1;
  }

  ilemu::HostResourceBudget shared_budget;
  shared_budget.worker_count = 2U;
  shared_budget.duty_period = 250ms;
  shared_budget.interactive_compile_budget = 20ms;
  shared_budget.deadline_reserve = 0ms;
  ilemu::HostResourceController shared_controller{shared_budget};
  std::mutex reservation_mutex;
  std::condition_variable reservation_condition;
  int started = 0;
  bool release_first = false;
  const auto first = shared_controller.submit(
      ilemu::HostWorkKind::BackgroundCompile, std::nullopt,
      [&] {
        std::unique_lock lock{reservation_mutex};
        ++started;
        reservation_condition.notify_all();
        reservation_condition.wait(lock, [&] { return release_first; });
      },
      15ms);
  const auto second = shared_controller.submit(
      ilemu::HostWorkKind::BackgroundCompile, std::nullopt,
      [&] {
        std::lock_guard lock{reservation_mutex};
        ++started;
        reservation_condition.notify_all();
      },
      15ms);
  if (!first || !second) {
    std::cerr << "budget reservation tasks were not accepted\n";
    {
      std::lock_guard lock{reservation_mutex};
      release_first = true;
    }
    reservation_condition.notify_all();
    shared_controller.wait_idle();
    return 1;
  }
  bool overlapped = false;
  {
    std::unique_lock lock{reservation_mutex};
    if (!reservation_condition.wait_for(lock, 100ms,
                                        [&] { return started >= 1; })) {
      std::cerr << "budget reservation first task did not start\n";
      release_first = true;
    } else {
      overlapped = reservation_condition.wait_for(
          lock, 100ms, [&] { return started >= 2; });
      release_first = true;
    }
  }
  reservation_condition.notify_all();
  shared_controller.wait_idle();
  if (overlapped || started != 2 || shared_controller.completed() != 2U) {
    std::cerr << "background workers bypassed the shared duty budget\n";
    return 1;
  }

  ilemu::HostResourceBudget split_budget;
  split_budget.worker_count = 2U;
  split_budget.duty_period = 250ms;
  split_budget.interactive_compile_budget = 5ms;
  split_budget.offline_compile_budget = 20ms;
  split_budget.deadline_reserve = 0ms;
  ilemu::HostResourceController split_controller{split_budget};
  std::mutex split_mutex;
  std::condition_variable split_condition;
  int split_started = 0;
  bool release_split = false;
  const auto split_work = [&] {
    std::unique_lock lock{split_mutex};
    ++split_started;
    split_condition.notify_all();
    split_condition.wait(lock, [&] { return release_split; });
  };
  const auto interactive = split_controller.submit(
      ilemu::HostWorkKind::BackgroundCompile, std::nullopt, split_work, 5ms);
  const auto offline = split_controller.submit(
      ilemu::HostWorkKind::OfflineCompile, std::nullopt, split_work, 20ms);
  if (!interactive || !offline) {
    std::cerr << "separate compile budgets rejected valid work\n";
    {
      std::lock_guard lock{split_mutex};
      release_split = true;
    }
    split_condition.notify_all();
    split_controller.wait_idle();
    return 1;
  }
  {
    std::unique_lock lock{split_mutex};
    if (!split_condition.wait_for(lock, 100ms,
                                  [&] { return split_started == 2; })) {
      std::cerr << "separate compile budgets serialized independent work\n";
      release_split = true;
    } else {
      release_split = true;
    }
  }
  split_condition.notify_all();
  split_controller.wait_idle();
  if (split_started != 2 || split_controller.completed() != 2U) {
    std::cerr << "separate compile budgets did not run independently\n";
    return 1;
  }

  ilemu::HostResourceBudget deadline_budget;
  deadline_budget.worker_count = 1U;
  deadline_budget.interactive_compile_budget = 20ms;
  deadline_budget.deadline_reserve = 2ms;
  ilemu::HostResourceController deadline_controller{deadline_budget};
  std::mutex deadline_mutex;
  std::condition_variable deadline_condition;
  bool deadline_blocker_started = false;
  bool release_deadline_blocker = false;
  bool deadline_background_ran = false;
  const auto deadline_blocker = deadline_controller.submit(
      ilemu::HostWorkKind::Maintenance, std::nullopt, [&] {
        std::unique_lock lock{deadline_mutex};
        deadline_blocker_started = true;
        deadline_condition.notify_all();
        deadline_condition.wait(lock,
                                [&] { return release_deadline_blocker; });
      });
  const auto initial_deadline =
      ilemu::HostResourceController::Clock::now() + 100ms;
  if (!deadline_blocker) {
    std::cerr << "deadline recheck tasks were not accepted\n";
    {
      std::lock_guard lock{deadline_mutex};
      release_deadline_blocker = true;
    }
    deadline_condition.notify_all();
    deadline_controller.wait_idle();
    return 1;
  }
  bool deadline_setup_failed = false;
  std::shared_ptr<ilemu::HostWorkToken> deadline_background;
  {
    std::unique_lock lock{deadline_mutex};
    if (!deadline_condition.wait_for(
            lock, 100ms, [&] { return deadline_blocker_started; })) {
      std::cerr << "deadline recheck blocker did not start\n";
      release_deadline_blocker = true;
      deadline_setup_failed = true;
    } else {
      deadline_background = deadline_controller.submit(
          ilemu::HostWorkKind::BackgroundCompile, initial_deadline,
          [&] {
            std::lock_guard lock{deadline_mutex};
            deadline_background_ran = true;
          },
          1ms);
      deadline_controller.set_next_deadline(
          ilemu::HostResourceController::Clock::now() + 1ms);
      release_deadline_blocker = true;
    }
  }
  deadline_condition.notify_all();
  if (deadline_setup_failed) {
    deadline_controller.set_next_deadline(std::nullopt);
    deadline_controller.wait_idle();
    return 1;
  }
  if (!deadline_background) {
    std::cerr << "deadline recheck task was not accepted\n";
    deadline_controller.set_next_deadline(std::nullopt);
    deadline_controller.wait_idle();
    return 1;
  }
  std::this_thread::sleep_for(10ms);
  bool deadline_was_ignored = false;
  {
    std::lock_guard lock{deadline_mutex};
    deadline_was_ignored = deadline_background_ran;
  }
  if (deadline_was_ignored) {
    std::cerr << "background task ignored the latest deadline\n";
    deadline_controller.set_next_deadline(std::nullopt);
    deadline_controller.wait_idle();
    return 1;
  }
  deadline_controller.set_next_deadline(std::nullopt);
  deadline_controller.wait_idle();
  if (!deadline_background_ran) {
    std::cerr << "background task did not run after deadline advanced\n";
    return 1;
  }
  ilemu::performance_counters().reset(false);
  ilemu::performance_counters().record_jit_block_compile(
      std::chrono::duration_cast<std::chrono::nanoseconds>(1ms).count());
  ilemu::performance_counters().record_jit_block_compile(
      std::chrono::duration_cast<std::chrono::nanoseconds>(3ms).count());
  if (ilemu::performance_counters().jit_block_compile_p95_nanoseconds() == 0 ||
      ilemu::performance_counters().jit_block_compile_p99_nanoseconds() == 0) {
    std::cerr << "JIT block compile history did not produce a reserve\n";
    return 1;
  }
  return 0;
}
