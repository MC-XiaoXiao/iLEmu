add_subdirectory(iokit)

ilemu_add_compatibility_suite(
    ilemu_kernel_tests kernel
    kernel_tests.cpp
    sysctl_tests.cpp
    device_tests.cpp)
target_link_libraries(ilemu_kernel_tests PRIVATE
    iLEmu::kernel_iokit_power_tests
    iLEmu::kernel_iokit_display_tests)

ilemu_add_compatibility_suite(
    ilemu_kernel_legacy_nosys_tests legacy_nosys
    legacy_nosys_tests.cpp)

ilemu_add_compatibility_suite(
    ilemu_shared_region_tests shared_region
    shared_region_tests.cpp)
