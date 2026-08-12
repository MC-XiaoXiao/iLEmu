add_subdirectory(vm)
add_subdirectory(task)
add_subdirectory(thread)

add_executable(ilemu_mach_model_tests mach_model_tests.cpp)
target_link_libraries(ilemu_mach_model_tests PRIVATE
    iLEmu::mach
    ilemu_test_support)
add_test(NAME mach_model COMMAND ilemu_mach_model_tests)
set_tests_properties(mach_model PROPERTIES
    WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}")

ilemu_add_compatibility_suite(
    ilemu_kernel_mach_tests kernel_mach
    main.cpp
    mig_policy_tests.cpp
    timer_tests.cpp
    semaphore_tests.cpp
    port_task_tests.cpp
    ipc_lifecycle_tests.cpp
    ipc_ool_tests.cpp
    launchd_ipc_tests.cpp
    port_limit_tests.cpp
    processor_set_tests.cpp)
target_link_libraries(ilemu_kernel_mach_tests PRIVATE
    iLEmu::kernel_mach_vm_tests
    iLEmu::kernel_mach_task_tests
    iLEmu::kernel_mach_thread_tests)
