#pragma once

namespace ilemu::test::mach_suite {

void run_mig_policy_tests();
void run_timer_tests();
void run_semaphore_tests();
void run_port_task_tests();
void run_ipc_lifecycle_tests();
void run_ipc_ool_tests();
void run_launchd_ipc_tests();
void run_port_limit_tests();
void run_processor_set_tests();
void run_task_tests();
void run_thread_tests();
void run_vm_tests();

} // namespace ilemu::test::mach_suite
