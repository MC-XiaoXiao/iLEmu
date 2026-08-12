#include "suite.hpp"
#include "test_support.hpp"

namespace ilemu::test::mach_suite {

void run_tests() {
  run_mig_policy_tests();
  run_timer_tests();
  run_semaphore_tests();
  run_port_task_tests();
  run_ipc_lifecycle_tests();
  run_ipc_ool_tests();
  run_launchd_ipc_tests();
  run_port_limit_tests();
  run_processor_set_tests();
  run_task_tests();
  run_thread_tests();
  run_vm_tests();
}

} // namespace ilemu::test::mach_suite

int main() {
  return ilemu::test::run_suite("mach_suite",
                                     ilemu::test::mach_suite::run_tests);
}
