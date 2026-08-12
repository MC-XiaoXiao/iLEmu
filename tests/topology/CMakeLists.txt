add_executable(ilemu_guest_topology_tests topology_tests.cpp)
target_link_libraries(ilemu_guest_topology_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
add_test(NAME guest_topology COMMAND ilemu_guest_topology_tests)
set_tests_properties(guest_topology PROPERTIES
    WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}")
