add_subdirectory(control)

add_library(ilemu_kernel_bsd_virtual_udp STATIC
    virtual_udp.cpp)
add_library(iLEmu::kernel_bsd_virtual_udp ALIAS
    ilemu_kernel_bsd_virtual_udp)
ilemu_configure_module(ilemu_kernel_bsd_virtual_udp)
target_link_libraries(ilemu_kernel_bsd_virtual_udp PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_virtual_udp_syscalls STATIC
    virtual_udp_syscalls.cpp)
add_library(iLEmu::kernel_bsd_virtual_udp_syscalls ALIAS
    ilemu_kernel_bsd_virtual_udp_syscalls)
ilemu_configure_module(ilemu_kernel_bsd_virtual_udp_syscalls)
target_link_libraries(ilemu_kernel_bsd_virtual_udp_syscalls PRIVATE
    iLEmu::kernel_bsd_virtual_udp
    iLEmu::foundation)
