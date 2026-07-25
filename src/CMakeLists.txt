# The adapter is generated from XNU MIG definitions and must exist before the
# foundation library declares its public dependency on it.
add_subdirectory(mach/adapter)
add_subdirectory(foundation)
add_subdirectory(device_state)
add_subdirectory(mach)
add_subdirectory(filesystem)
add_subdirectory(network)
add_subdirectory(storage)
add_subdirectory(telephony)
add_subdirectory(media)
add_subdirectory(bluetooth)
add_subdirectory(graphics)
add_subdirectory(kernel)
add_subdirectory(debug)

add_library(ilemu_core INTERFACE)
add_library(iLEmu::core ALIAS ilemu_core)
target_link_libraries(ilemu_core INTERFACE
    iLEmu::kernel
    iLEmu::debug)
