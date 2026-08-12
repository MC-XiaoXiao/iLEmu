add_executable(ilemu_resolver_contract_tests resolver_contract_tests.cpp)
target_link_libraries(ilemu_resolver_contract_tests PRIVATE
    iLEmu::network
    iLEmu::foundation
    ilemu_test_support)
add_test(NAME resolver_contract COMMAND ilemu_resolver_contract_tests)
set_tests_properties(resolver_contract PROPERTIES
    WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}")

ilemu_add_compatibility_suite(
    ilemu_network_tests network
    main.cpp
    abi_route_tests.cpp
    host_socket_tests.cpp
    wifi_tests.cpp
    virtual_udp_tests.cpp
    event_tests.cpp
    kernel_control_tests.cpp
    unix_socket_tests.cpp)
