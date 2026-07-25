add_library(ilemu_kernel_bsd_debug STATIC
    tracing.cpp)
add_library(iLEmu::kernel_bsd_debug ALIAS
    ilemu_kernel_bsd_debug)
ilemu_configure_module(ilemu_kernel_bsd_debug)
target_link_libraries(ilemu_kernel_bsd_debug PRIVATE
    iLEmu::foundation)
