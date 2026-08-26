add_library(ilemu_kernel_iokit_hid STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_hid ALIAS
    ilemu_kernel_iokit_hid)
ilemu_configure_module(ilemu_kernel_iokit_hid)
target_link_libraries(ilemu_kernel_iokit_hid PRIVATE
    iLEmu::foundation)
