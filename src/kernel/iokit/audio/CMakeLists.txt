add_library(ilemu_kernel_iokit_audio STATIC
    device_profile.cpp
    profile.cpp
    service.cpp)
add_library(iLEmu::kernel_iokit_audio ALIAS
    ilemu_kernel_iokit_audio)
ilemu_configure_module(ilemu_kernel_iokit_audio)
target_link_libraries(ilemu_kernel_iokit_audio PRIVATE
    iLEmu::foundation)
