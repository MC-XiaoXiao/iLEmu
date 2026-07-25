add_library(ilemu_bluetooth STATIC
    bluetooth_manager_hle.cpp)
add_library(iLEmu::bluetooth ALIAS ilemu_bluetooth)
ilemu_configure_module(ilemu_bluetooth)
target_link_libraries(ilemu_bluetooth PUBLIC iLEmu::foundation)
