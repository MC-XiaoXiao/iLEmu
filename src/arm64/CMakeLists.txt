find_path(ILEMU_OAKNUT_INCLUDE_DIR oaknut/oaknut.hpp
    HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../external/dynarmic/externals/oaknut/include"
    DOC "Independent Oaknut assembler include directory"
    NO_CMAKE_FIND_ROOT_PATH)
if(NOT ILEMU_OAKNUT_INCLUDE_DIR)
    message(FATAL_ERROR "Oaknut headers required; set ILEMU_OAKNUT_INCLUDE_DIR")
endif()
add_library(ilemu_arm64 STATIC address_register_plan.cpp address_registers.cpp address_segments.cpp trace_views.cpp continuation_scope.cpp native_continuations.cpp shared_compilation_cache.cpp code_cache.cpp compiler.cpp scalar_register_plan.cpp shift_count_range.cpp instruction_window.cpp flag_effects.cpp system_emitter.cpp exclusive_emitter.cpp executor.cpp integer_emitter.cpp logical_emitter.cpp register_shift_emitter.cpp multiply_emitter.cpp dsp_multiply_emitter.cpp dual_halfword_multiply_emitter.cpp transform_emitter.cpp parallel_saturating_emitter.cpp parallel_arithmetic_emitter.cpp parallel_byte_arithmetic_emitter.cpp saturating_arithmetic_emitter.cpp scalar_saturation_emitter.cpp memory_emitter.cpp address_cache.cpp address_windows.cpp multiple_emitter.cpp simd_emitter.cpp simd_reverse_emitter.cpp simd_shift_emitter.cpp simd_conversion_emitter.cpp simd_floating_emitter.cpp simd_integer_emitter.cpp simd_immediate_emitter.cpp core_vector_move_emitter.cpp floating_immediate_emitter.cpp floating_comparison_emitter.cpp floating_emitter.cpp floating_arithmetic_emitter.cpp vector_memory_emitter.cpp vector_split_emitter.cpp vfp_memory_emitter.cpp)
add_library(iLEmu::arm64 ALIAS ilemu_arm64)
target_link_libraries(ilemu_arm64 PUBLIC iLEmu::execution PRIVATE iLEmu::arm iLEmu::arm_memory iLEmu::arm_interpreter)
target_include_directories(ilemu_arm64 PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_include_directories(ilemu_arm64 SYSTEM PRIVATE "${ILEMU_OAKNUT_INCLUDE_DIR}")
target_compile_options(ilemu_arm64 PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wconversion;-Wshadow>)
