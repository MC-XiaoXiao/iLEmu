add_library(ilemu_kernel_bsd_security STATIC
    mac.cpp
    sandbox.cpp)
add_library(iLEmu::kernel_bsd_security ALIAS
    ilemu_kernel_bsd_security)
ilemu_configure_module(ilemu_kernel_bsd_security)
target_link_libraries(ilemu_kernel_bsd_security PRIVATE
    iLEmu::foundation)
