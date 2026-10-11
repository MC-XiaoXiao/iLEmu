find_package(Threads REQUIRED)

add_library(ilemu_host_execution STATIC code_memory.cpp code_pool.cpp)
add_library(iLEmu::host_execution ALIAS ilemu_host_execution)
target_link_libraries(ilemu_host_execution
    PUBLIC iLEmu::execution PRIVATE Threads::Threads)
target_include_directories(ilemu_host_execution PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_include_directories(ilemu_host_execution SYSTEM PRIVATE
    "${ILEMU_OAKNUT_INCLUDE_DIR}")
