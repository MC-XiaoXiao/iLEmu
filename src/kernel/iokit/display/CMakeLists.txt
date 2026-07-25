add_library(ilemu_kernel_iokit_display STATIC
    vsync.cpp)
add_library(iLEmu::kernel_iokit_display ALIAS
    ilemu_kernel_iokit_display)
ilemu_configure_module(ilemu_kernel_iokit_display)
target_link_libraries(ilemu_kernel_iokit_display PRIVATE
    iLEmu::foundation)
