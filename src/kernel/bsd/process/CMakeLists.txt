add_library(ilemu_kernel_bsd_process STATIC
    credentials.cpp
    spawn.cpp)
add_library(iLEmu::kernel_bsd_process ALIAS
    ilemu_kernel_bsd_process)
ilemu_configure_module(ilemu_kernel_bsd_process)
target_link_libraries(ilemu_kernel_bsd_process PRIVATE
    iLEmu::foundation)
