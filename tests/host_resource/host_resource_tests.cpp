#include "ilemu/host_resource_controller.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
  using namespace std::chrono_literals;
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
  return 0;
}
