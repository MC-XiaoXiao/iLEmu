add_library(ilemu_app_runtime STATIC
    guest_parallelism_policy.cpp
    realtime_pacer.cpp)
add_library(iLEmu::app_runtime ALIAS ilemu_app_runtime)
ilemu_configure_module(ilemu_app_runtime)
target_link_libraries(ilemu_app_runtime PUBLIC
    iLEmu::foundation)
