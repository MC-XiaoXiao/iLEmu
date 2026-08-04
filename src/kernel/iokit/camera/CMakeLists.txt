add_library(ilemu_kernel_iokit_camera STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_camera ALIAS
    ilemu_kernel_iokit_camera)
ilemu_configure_module(ilemu_kernel_iokit_camera)
target_link_libraries(ilemu_kernel_iokit_camera PRIVATE
    iLEmu::foundation)
