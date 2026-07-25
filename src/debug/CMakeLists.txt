add_library(ilemu_debug STATIC gdb_rsp.cpp)
add_library(iLEmu::debug ALIAS ilemu_debug)
ilemu_configure_module(ilemu_debug)
target_link_libraries(ilemu_debug PUBLIC iLEmu::foundation)
