# Berkeley SoftFloat Release 3e, official source archive:
# https://www.jhauser.us/arithmetic/SoftFloat-3e.zip
# Only the dependency closure for scalar int32/float32/float64 conversions.
if(NOT CMAKE_C_COMPILER_LOADED)
    enable_language(C)
endif()
add_library(ilemu_softfloat STATIC
    source/f32_to_f64.c
    source/f32_to_i32.c
    source/f32_to_ui32.c
    source/f64_to_f32.c
    source/f64_to_i32.c
    source/f64_to_ui32.c
    source/i32_to_f32.c
    source/i32_to_f64.c
    source/ARM-VFPv2/s_commonNaNToF32UI.c
    source/ARM-VFPv2/s_commonNaNToF64UI.c
    source/s_countLeadingZeros32.c
    source/s_countLeadingZeros8.c
    source/ARM-VFPv2/s_f32UIToCommonNaN.c
    source/ARM-VFPv2/s_f64UIToCommonNaN.c
    source/s_normRoundPackToF32.c
    source/s_normSubnormalF32Sig.c
    source/s_roundPackToF32.c
    source/s_roundToI32.c
    source/s_roundToUI32.c
    source/s_shiftRightJam32.c
    source/s_shiftRightJam64.c
    source/s_shortShiftRightJam64.c
    source/ARM-VFPv2/softfloat_raiseFlags.c
    source/softfloat_state.c
    source/ui32_to_f32.c
    source/ui32_to_f64.c
)
target_include_directories(ilemu_softfloat PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/source/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/ARM-VFPv2")
target_compile_definitions(ilemu_softfloat PRIVATE
    SOFTFLOAT_FAST_INT64 THREAD_LOCAL=_Thread_local)
set_target_properties(ilemu_softfloat PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
