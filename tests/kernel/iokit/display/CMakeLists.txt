add_library(ilemu_kernel_iokit_display_tests STATIC
    vsync_tests.cpp)
add_library(iLEmu::kernel_iokit_display_tests ALIAS
    ilemu_kernel_iokit_display_tests)
ilemu_configure_module(ilemu_kernel_iokit_display_tests)
target_link_libraries(ilemu_kernel_iokit_display_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
