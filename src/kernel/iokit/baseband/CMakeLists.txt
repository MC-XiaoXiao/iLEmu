add_library(ilemu_kernel_iokit_baseband STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_baseband ALIAS
    ilemu_kernel_iokit_baseband)
ilemu_configure_module(ilemu_kernel_iokit_baseband)
target_link_libraries(ilemu_kernel_iokit_baseband PRIVATE
    iLEmu::foundation)
