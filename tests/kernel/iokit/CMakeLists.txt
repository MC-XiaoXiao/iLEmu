add_subdirectory(display)

add_library(ilemu_kernel_iokit_power_tests STATIC
    power_tests.cpp)
add_library(iLEmu::kernel_iokit_power_tests ALIAS
    ilemu_kernel_iokit_power_tests)
ilemu_configure_module(ilemu_kernel_iokit_power_tests)
target_link_libraries(ilemu_kernel_iokit_power_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
