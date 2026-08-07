#include "ilemu/host_resource_controller.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "ilemu/deadline_queue.hpp"

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
      });
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
                        [] {}) != nullptr ||
      controller.rejected() == 0U) {
    std::cerr << "near-deadline compile work was not rejected\n";
    return 1;
  }
  controller.set_next_deadline(std::nullopt);
  const auto retry = controller.submit(
      ilemu::HostWorkKind::BackgroundCompile, std::nullopt, [] {});
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
      });
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
  return 0;
}
