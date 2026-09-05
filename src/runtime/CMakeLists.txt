add_library(ilemu_runtime STATIC
    boot_options.cpp
    emulator_session.cpp
    realtime_pacer.cpp
    resource_policy.cpp
    input/live_button_scheduler.cpp
    input/live_touch_scheduler.cpp)
add_library(iLEmu::runtime ALIAS ilemu_runtime)
ilemu_configure_module(ilemu_runtime)
target_link_libraries(ilemu_runtime PUBLIC iLEmu::core)
