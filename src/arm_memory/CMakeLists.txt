add_library(ilemu_arm_memory STATIC transfer.cpp multiple.cpp vector_transfer.cpp)
add_library(iLEmu::arm_memory ALIAS ilemu_arm_memory)
target_link_libraries(ilemu_arm_memory PUBLIC iLEmu::execution iLEmu::arm)
target_include_directories(ilemu_arm_memory PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_options(ilemu_arm_memory PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wconversion;-Wshadow>)
