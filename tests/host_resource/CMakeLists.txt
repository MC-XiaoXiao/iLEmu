add_executable(ilemu_host_resource_tests host_resource_tests.cpp)
target_link_libraries(ilemu_host_resource_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
add_test(NAME host_resource COMMAND ilemu_host_resource_tests)
set_tests_properties(host_resource PROPERTIES
    WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}")
