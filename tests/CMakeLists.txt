add_subdirectory(support)

function(ilemu_add_compatibility_suite target test_name source)
    add_executable(${target} ${source} ${ARGN})
    target_link_libraries(${target} PRIVATE
        iLEmu::core
        ilemu_test_support)
    add_test(NAME ${test_name} COMMAND ${target})
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}")
endfunction()

add_subdirectory(kernel)
add_subdirectory(graphics)
add_subdirectory(filesystem)
add_subdirectory(network)
add_subdirectory(mach)
add_subdirectory(integration)
add_subdirectory(topology)
add_subdirectory(catalog)
add_subdirectory(file_cache)
add_subdirectory(dyld_cache)
add_subdirectory(jit_profile)
add_subdirectory(jit_artifact)
add_subdirectory(host_resource)
