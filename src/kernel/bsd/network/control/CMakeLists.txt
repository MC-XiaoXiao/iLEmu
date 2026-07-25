add_library(ilemu_kernel_bsd_control STATIC
    control.cpp
    syscalls.cpp)
add_library(iLEmu::kernel_bsd_control ALIAS
    ilemu_kernel_bsd_control)
ilemu_configure_module(ilemu_kernel_bsd_control)
target_link_libraries(ilemu_kernel_bsd_control PRIVATE
    iLEmu::foundation)
