add_library(ilemu_kernel_api INTERFACE)
add_library(iLEmu::kernel_api ALIAS ilemu_kernel_api)
target_include_directories(ilemu_kernel_api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(ilemu_kernel_api INTERFACE
    iLEmu::device_state
    iLEmu::foundation
    iLEmu::mach
    iLEmu::filesystem
    iLEmu::network
    iLEmu::storage
    iLEmu::telephony
    iLEmu::media
    iLEmu::bluetooth
    iLEmu::crypto
    iLEmu::graphics
    iLEmu::xnu_adapter)

add_library(ilemu_kernel STATIC
    kernel.cpp
    clock.cpp
    iokit.cpp
    network.cpp)
add_library(iLEmu::kernel ALIAS ilemu_kernel)
ilemu_configure_module(ilemu_kernel)

add_subdirectory(graphics)
add_subdirectory(bsd)
add_subdirectory(iokit)
add_subdirectory(mach)

target_link_libraries(ilemu_kernel PRIVATE
    iLEmu::kernel_graphics
    iLEmu::device_state
    iLEmu::kernel_iokit_audio
    iLEmu::kernel_iokit_baseband
    iLEmu::kernel_iokit_camera
    iLEmu::kernel_iokit_power
    iLEmu::kernel_iokit_display
    iLEmu::kernel_iokit_graphics
    iLEmu::kernel_iokit_hid
    iLEmu::kernel_iokit_jpeg
    iLEmu::kernel_iokit_keybag
    iLEmu::kernel_iokit_mbx
    iLEmu::kernel_iokit_mobile_file_integrity)
