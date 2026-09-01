add_library(ilemu_kernel_bsd_baseband_device STATIC
    device.cpp
    offline_control.cpp
    replay.cpp)
add_library(iLEmu::kernel_bsd_baseband_device ALIAS
    ilemu_kernel_bsd_baseband_device)
ilemu_configure_module(ilemu_kernel_bsd_baseband_device)
target_link_libraries(ilemu_kernel_bsd_baseband_device PRIVATE
    iLEmu::foundation)
