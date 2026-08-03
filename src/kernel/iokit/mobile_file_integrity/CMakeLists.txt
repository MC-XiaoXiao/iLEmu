add_library(ilemu_kernel_iokit_mobile_file_integrity STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_mobile_file_integrity ALIAS
    ilemu_kernel_iokit_mobile_file_integrity)
ilemu_configure_module(ilemu_kernel_iokit_mobile_file_integrity)
target_link_libraries(ilemu_kernel_iokit_mobile_file_integrity PRIVATE
    iLEmu::foundation)
