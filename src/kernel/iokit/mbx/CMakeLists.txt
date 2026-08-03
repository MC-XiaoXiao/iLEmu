add_library(ilemu_kernel_iokit_mbx STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_mbx ALIAS
    ilemu_kernel_iokit_mbx)
ilemu_configure_module(ilemu_kernel_iokit_mbx)
target_link_libraries(ilemu_kernel_iokit_mbx PRIVATE
    iLEmu::foundation)
