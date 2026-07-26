add_library(ilemu_storage STATIC
    app_support_hle.cpp
    lockdown_hle.cpp)
add_library(iLEmu::storage ALIAS ilemu_storage)
ilemu_configure_module(ilemu_storage)
target_link_libraries(ilemu_storage PUBLIC iLEmu::foundation)
