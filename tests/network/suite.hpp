#pragma once

namespace ilemu::test::network_suite {

void run_abi_route_tests();
void run_host_socket_tests();
void run_wifi_tests();
void run_virtual_udp_tests();
void run_event_tests();
void run_kernel_control_tests();
void run_unix_socket_tests();

} // namespace ilemu::test::network_suite
