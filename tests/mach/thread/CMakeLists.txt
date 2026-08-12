add_library(ilemu_kernel_mach_thread_tests STATIC
    state_tests.cpp)
add_library(iLEmu::kernel_mach_thread_tests ALIAS
    ilemu_kernel_mach_thread_tests)
ilemu_configure_module(ilemu_kernel_mach_thread_tests)
target_link_libraries(ilemu_kernel_mach_thread_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
