add_library(ilemu_kernel STATIC
    kernel.cpp
    clock.cpp
    iokit.cpp
    network.cpp)
add_library(iLEmu::kernel ALIAS ilemu_kernel)
ilemu_configure_module(ilemu_kernel)
target_link_libraries(ilemu_kernel PUBLIC
    iLEmu::foundation
    iLEmu::mach
    iLEmu::filesystem
    iLEmu::network
    iLEmu::storage
    iLEmu::telephony
    iLEmu::media
    iLEmu::bluetooth
    iLEmu::graphics
    iLEmu::xnu_adapter)

add_subdirectory(bsd)
add_subdirectory(iokit)
add_subdirectory(mach)

target_link_libraries(ilemu_kernel PRIVATE
    iLEmu::kernel_iokit_audio
    iLEmu::kernel_iokit_baseband
    iLEmu::kernel_iokit_power
    iLEmu::kernel_iokit_display
    iLEmu::kernel_iokit_mobile_file_integrity)
