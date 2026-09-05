add_library(ilemu_kernel_bsd_filesystem_control STATIC
    control.cpp)
add_library(iLEmu::kernel_bsd_filesystem_control ALIAS
    ilemu_kernel_bsd_filesystem_control)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_control)
target_link_libraries(ilemu_kernel_bsd_filesystem_control PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_ownership STATIC
    ownership.cpp)
add_library(iLEmu::kernel_bsd_filesystem_ownership ALIAS
    ilemu_kernel_bsd_filesystem_ownership)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_ownership)
target_link_libraries(ilemu_kernel_bsd_filesystem_ownership PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_locking STATIC
    locking.cpp)
add_library(iLEmu::kernel_bsd_filesystem_locking ALIAS
    ilemu_kernel_bsd_filesystem_locking)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_locking)
target_link_libraries(ilemu_kernel_bsd_filesystem_locking PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_persistence STATIC
    persistence.cpp)
add_library(iLEmu::kernel_bsd_filesystem_persistence ALIAS
    ilemu_kernel_bsd_filesystem_persistence)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_persistence)
target_link_libraries(ilemu_kernel_bsd_filesystem_persistence PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_timestamps STATIC
    timestamps.cpp)
add_library(iLEmu::kernel_bsd_filesystem_timestamps ALIAS
    ilemu_kernel_bsd_filesystem_timestamps)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_timestamps)
target_link_libraries(ilemu_kernel_bsd_filesystem_timestamps PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)
