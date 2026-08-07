add_library(ilemu_foundation_memory STATIC
    ../content_identity.cpp
    file_page_cache.cpp
    vm_map.cpp)
add_library(iLEmu::foundation_memory ALIAS ilemu_foundation_memory)
ilemu_configure_module(ilemu_foundation_memory)
