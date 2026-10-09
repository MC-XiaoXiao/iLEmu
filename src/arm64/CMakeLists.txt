find_path(ILEMU_OAKNUT_INCLUDE_DIR oaknut/oaknut.hpp
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../external/dynarmic/externals/oaknut/include"
    DOC "Independent Oaknut assembler include directory"
    NO_CMAKE_FIND_ROOT_PATH)
if(NOT ILEMU_OAKNUT_INCLUDE_DIR)
    message(FATAL_ERROR "Oaknut headers required; set ILEMU_OAKNUT_INCLUDE_DIR")
endif()
add_library(ilemu_arm64 STATIC code_cache.cpp compiler.cpp executor.cpp integer_emitter.cpp memory_emitter.cpp multiple_emitter.cpp simd_emitter.cpp vector_memory_emitter.cpp)
add_library(iLEmu::arm64 ALIAS ilemu_arm64)
target_link_libraries(ilemu_arm64 PUBLIC iLEmu::execution PRIVATE iLEmu::arm iLEmu::arm_memory)
target_include_directories(ilemu_arm64 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_include_directories(ilemu_arm64 SYSTEM PRIVATE "${ILEMU_OAKNUT_INCLUDE_DIR}")
target_compile_options(ilemu_arm64 PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wconversion;-Wshadow>)
