add_library(ilemu_filesystem STATIC hfs_metadata.cpp hfs_volume_profile.cpp)
add_library(iLEmu::filesystem ALIAS ilemu_filesystem)
ilemu_configure_module(ilemu_filesystem)
target_link_libraries(ilemu_filesystem PUBLIC iLEmu::foundation)
