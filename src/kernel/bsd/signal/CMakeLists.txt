add_library(ilemu_kernel_bsd_signal STATIC
    signal.cpp)
add_library(iLEmu::kernel_bsd_signal ALIAS
    ilemu_kernel_bsd_signal)
ilemu_configure_module(ilemu_kernel_bsd_signal)
target_link_libraries(ilemu_kernel_bsd_signal PRIVATE
    iLEmu::foundation)
