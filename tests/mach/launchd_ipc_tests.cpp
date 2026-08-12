#include "suite.hpp"

#include "test_support.hpp"

#include "ilemu/address_space.hpp"
#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/mach_namespace.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/task_mig_ids.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <sstream>

namespace ilemu::test::mach_suite {
namespace {

using ::ilemu::test::require;

constexpr std::uint32_t mach_send = 1;
constexpr std::uint32_t mach_receive = 2;
constexpr std::uint32_t mach_send_receive = mach_send | mach_receive;
constexpr std::uint32_t kernel_reply_name = 0x900U;

std::uint32_t allocate_receive_right(CompatibilityKernel &kernel, Cpu &cpu) {
  cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  kernel.dispatch(cpu, 0x80);
  const auto name = cpu.registers()[0];
  require(name != 0, "launchd receive-right allocation failed");
  return name;
}

void allocate_send_receive_right(CompatibilityKernel &kernel, Cpu &cpu,
                                 AddressSpace &memory, std::uint32_t buffer,
                                 std::uint32_t name) {
  const auto insert_right_id = xnu792::mig::mach_port::id(
      xnu792::mig::mach_port::Routine::mach_port_insert_right);
  require(
      memory.write32(buffer,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once, true)) &&
          memory.write32(buffer + 4, 52) &&
          memory.write32(buffer + 8, kernel.process().task_port) &&
          memory.write32(buffer + 12, kernel_reply_name) &&
          memory.write32(buffer + 20, insert_right_id) &&
          memory.write32(buffer + 24, 1) && memory.write32(buffer + 28, name) &&
          memory.write32(buffer + 32, 0) &&
          memory.write32(buffer + 36,
                         darwin::mig_wire::port_descriptor_metadata(
                             darwin::mig_wire::disposition_make_send)) &&
          memory.write32(buffer + 40, 0) && memory.write32(buffer + 44, 1) &&
          memory.write32(buffer + 48, name),
      "launchd service send-right insertion setup failed");
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  cpu.registers()[0] = buffer;
  cpu.registers()[1] = mach_send_receive;
  cpu.registers()[2] = 52;
  cpu.registers()[3] = 64;
  cpu.registers()[4] = kernel_reply_name;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read32(buffer + 32) == std::optional<std::uint32_t>{0},
          "launchd service send-right insertion failed");
}

std::uint32_t port_type(CompatibilityKernel &kernel, Cpu &cpu,
                        AddressSpace &memory, std::uint32_t buffer,
                        std::uint32_t name) {
  const auto port_type_id = xnu792::mig::mach_port::id(
      xnu792::mig::mach_port::Routine::mach_port_type);
  require(memory.write32(buffer, 0x1513U) && memory.write32(buffer + 4, 36) &&
              memory.write32(buffer + 8, kernel.process().task_port) &&
              memory.write32(buffer + 12, kernel_reply_name) &&
              memory.write32(buffer + 20, port_type_id) &&
              memory.write32(buffer + 32, name),
          "launchd OOL port type request setup failed");
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  cpu.registers()[0] = buffer;
  cpu.registers()[1] = mach_send_receive;
  cpu.registers()[2] = 36;
  cpu.registers()[3] = 64;
  cpu.registers()[4] = kernel_reply_name;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read32(buffer + 32) == std::optional<std::uint32_t>{0},
          "launchd OOL port type request failed");
  return memory.read32(buffer + 36).value_or(0);
}

std::uint32_t task_for_pid_name(CompatibilityKernel &caller, Cpu &cpu,
                                AddressSpace &memory,
                                std::uint32_t output_address,
                                std::uint32_t pid) {
  cpu.registers()[0] = caller.process().task_port;
  cpu.registers()[1] = pid;
  cpu.registers()[2] = output_address;
  cpu.registers()[12] = static_cast<std::uint32_t>(-45);
  caller.dispatch(cpu, 0x80);
  const auto name = memory.read32(output_address).value_or(0);
  require(cpu.registers()[0] == 0 && name != 0,
          "launchd child task capability copyout failed");
  return name;
}

void set_bootstrap_special_port(CompatibilityKernel &caller, Cpu &cpu,
                                AddressSpace &memory, std::uint32_t buffer,
                                std::uint32_t task_name,
                                std::uint32_t bootstrap_name) {
  const auto set_special_id =
      xnu792::mig::task::id(xnu792::mig::task::Routine::task_set_special_port);
  require(
      memory.write32(buffer,
                     darwin::mig_wire::message_bits(
                         darwin::mig_wire::disposition_copy_send,
                         darwin::mig_wire::disposition_make_send_once, true)) &&
          memory.write32(buffer + 4, 52) &&
          memory.write32(buffer + 8, task_name) &&
          memory.write32(buffer + 12, kernel_reply_name) &&
          memory.write32(buffer + 20, set_special_id) &&
          memory.write32(buffer + 24, 1) &&
          memory.write32(buffer + 28, bootstrap_name) &&
          memory.write32(buffer + 36,
                         darwin::mig_wire::port_descriptor_metadata(
                             darwin::mig_wire::disposition_copy_send)) &&
          memory.write32(buffer + 40, 0) && memory.write32(buffer + 44, 1) &&
          memory.write32(buffer + 48, 4),
      "launchd child bootstrap special-port setup failed");
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  cpu.registers()[0] = buffer;
  cpu.registers()[1] = mach_send_receive;
  cpu.registers()[2] = 52;
  cpu.registers()[3] = 64;
  cpu.registers()[4] = kernel_reply_name;
  caller.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 &&
              memory.read32(buffer + 32) == std::optional<std::uint32_t>{0},
          "launchd child bootstrap special-port assignment failed");
}

std::uint32_t get_bootstrap_special_port(CompatibilityKernel &kernel, Cpu &cpu,
                                         AddressSpace &memory,
                                         std::uint32_t buffer) {
  const auto get_special_id =
      xnu792::mig::task::id(xnu792::mig::task::Routine::task_get_special_port);
  require(memory.write32(buffer,
                         darwin::mig_wire::message_bits(
                             darwin::mig_wire::disposition_copy_send,
                             darwin::mig_wire::disposition_make_send_once)) &&
              memory.write32(buffer + 4, 36) &&
              memory.write32(buffer + 8, kernel.process().task_port) &&
              memory.write32(buffer + 12, kernel_reply_name) &&
              memory.write32(buffer + 20, get_special_id) &&
              memory.write32(buffer + 32, 4),
          "launchd bootstrap special-port query setup failed");
  cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  cpu.registers()[0] = buffer;
  cpu.registers()[1] = mach_send_receive;
  cpu.registers()[2] = 36;
  cpu.registers()[3] = 64;
  cpu.registers()[4] = kernel_reply_name;
  kernel.dispatch(cpu, 0x80);
  const auto name = memory.read32(buffer + 28).value_or(0);
  require(cpu.registers()[0] == 0 && name != 0 &&
              kernel.process().bootstrap_port == name,
          "launchd bootstrap special-port query failed");
  return name;
}

void transfer_subset_ool_ports_test() {
  AddressSpace server_memory;
  AddressSpace client_memory;
  constexpr std::uint32_t server_buffer = 0x41000U;
  constexpr std::uint32_t client_buffer = 0x42000U;
  constexpr std::uint32_t server_port_array = 0x43000U;
  require(
      server_memory.map(server_buffer, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          server_memory.map(server_port_array, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write) &&
          client_memory.map(client_buffer, AddressSpace::page_size,
                            MemoryPermission::Read | MemoryPermission::Write),
      "launchd OOL port buffers failed to map");

  Dynarmic::ExclusiveMonitor server_monitor{1};
  Dynarmic::ExclusiveMonitor client_monitor{1};
  Cpu server_cpu{0, server_memory, server_monitor};
  Cpu client_cpu{0, client_memory, client_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel server{server_memory, output};
  CompatibilityKernel client{client_memory, output};
  client.inherit_process_state(server, 2);

  client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  client.dispatch(client_cpu, 0x80);
  const auto client_reply = client_cpu.registers()[0];
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
  client.dispatch(client_cpu, 0x80);
  const auto client_collision_guard = client_cpu.registers()[0];
  require(client_reply != 0 && client_collision_guard != 0,
          "launchd client receive rights were not allocated");
  // Reserve two more local names so this test proves numeric names are
  // rewritten rather than merely observing a valid same-index coincidence
  // between independent ipc_spaces.
  constexpr std::size_t collision_guard_count = 2;
  for (std::size_t index = 0; index < collision_guard_count; ++index) {
    client_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
    client.dispatch(client_cpu, 0x80);
    require(client_cpu.registers()[0] != 0,
            "launchd namespace collision guard allocation failed");
  }

  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = mach_receive;
  server_cpu.registers()[2] = 0;
  server_cpu.registers()[3] = 128;
  server_cpu.registers()[4] = server.process().bootstrap_port;
  server.dispatch(server_cpu, 0x80);

  const auto transfer_subset_id = xnu792::mig::bootstrap::id(
      xnu792::mig::bootstrap::Routine::transfer_subset);
  require(
      client_memory.write32(
          client_buffer, darwin::mig_wire::message_bits(
                             darwin::mig_wire::disposition_copy_send,
                             darwin::mig_wire::disposition_make_send_once)) &&
          client_memory.write32(client_buffer + 4, 24) &&
          client_memory.write32(client_buffer + 8,
                                client.process().bootstrap_port) &&
          client_memory.write32(client_buffer + 12, client_reply) &&
          client_memory.write32(client_buffer + 20, transfer_subset_id),
      "launchd transfer_subset request setup failed");
  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_send;
  client_cpu.registers()[2] = 24;
  client.dispatch(client_cpu, 0x80);
  require(client_cpu.registers()[0] == 0 &&
              server.deliver_pending_mach(server_cpu),
          "launchd transfer_subset request was not delivered");
  const auto server_reply = server_memory.read32(server_buffer + 8).value_or(0);
  require(server_reply != 0, "launchd reply right was not copied out");

  std::array<std::uint32_t, 2> service_ports{};
  for (auto &service_port : service_ports) {
    server_cpu.registers()[12] = static_cast<std::uint32_t>(-26);
    server.dispatch(server_cpu, 0x80);
    service_port = server_cpu.registers()[0];
    require(service_port != 0, "launchd service port allocation failed");
    allocate_send_receive_right(server, server_cpu, server_memory,
                                server_buffer, service_port);
  }
  require(server_memory.write32(server_port_array, service_ports[0]) &&
              server_memory.write32(server_port_array + 4, 0) &&
              server_memory.write32(server_port_array + 8, service_ports[1]),
          "launchd service port array setup failed");

  client_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  client_cpu.registers()[0] = client_buffer;
  client_cpu.registers()[1] = mach_receive;
  client_cpu.registers()[2] = 0;
  client_cpu.registers()[3] = 128;
  client_cpu.registers()[4] = client_reply;
  client.dispatch(client_cpu, 0x80);

  constexpr std::uint32_t reply_size = 52;
  constexpr std::uint32_t port_count = 3;
  require(server_memory.write32(
              server_buffer,
              darwin::mig_wire::message_bits(
                  darwin::mig_wire::disposition_move_send_once, 0, true)) &&
              server_memory.write32(server_buffer + 4, reply_size) &&
              server_memory.write32(server_buffer + 8, server_reply) &&
              server_memory.write32(server_buffer + 12, 0) &&
              server_memory.write32(server_buffer + 20,
                                    transfer_subset_id + 100U) &&
              server_memory.write32(server_buffer + 24, 1) &&
              server_memory.write32(server_buffer + 28, server_port_array) &&
              server_memory.write32(server_buffer + 32, port_count) &&
              server_memory.write32(
                  server_buffer + 36,
                  darwin::mig_wire::ool_ports_descriptor_metadata(
                      darwin::mig_wire::disposition_copy_send, true)) &&
              server_memory.write32(server_buffer + 40, 0) &&
              server_memory.write32(server_buffer + 44, 1) &&
              server_memory.write32(server_buffer + 48, 0),
          "launchd transfer_subset OOL reply setup failed");
  server_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  server_cpu.registers()[0] = server_buffer;
  server_cpu.registers()[1] = mach_send;
  server_cpu.registers()[2] = reply_size;
  server.dispatch(server_cpu, 0x80);
  require(server_cpu.registers()[0] == 0 &&
              !server_memory.mapped(server_port_array) &&
              client.deliver_pending_mach(client_cpu),
          "launchd transfer_subset OOL reply was not transferred");

  const auto client_port_array =
      client_memory.read32(client_buffer + 28).value_or(0);
  const auto client_service_0 =
      client_memory.read32(client_port_array).value_or(0);
  const auto client_null =
      client_memory.read32(client_port_array + 4).value_or(1);
  const auto client_service_1 =
      client_memory.read32(client_port_array + 8).value_or(0);
  const auto send_mask = xnu792::ipc::type_mask(xnu792::ipc::Right::Send);
  const auto send_receive_mask =
      send_mask | xnu792::ipc::type_mask(xnu792::ipc::Right::Receive);
  require(client_port_array != 0 && client_port_array != server_port_array &&
              client_service_0 != 0 && client_service_1 != 0 &&
              client_service_0 != client_service_1 && client_null == 0 &&
              client_service_0 != service_ports[0] &&
              client_service_1 != service_ports[1],
          "launchd OOL port array was not copied into the client namespace");
  require(port_type(client, client_cpu, client_memory, client_buffer,
                    client_service_0) == send_mask &&
              port_type(client, client_cpu, client_memory, client_buffer,
                        client_service_1) == send_mask &&
              port_type(server, server_cpu, server_memory, server_buffer,
                        service_ports[0]) == send_receive_mask &&
              port_type(server, server_cpu, server_memory, server_buffer,
                        service_ports[1]) == send_receive_mask &&
              stream.str().find("mig=bootstrap.transfer_subset") !=
                  std::string::npos &&
              stream.str().find("ool-ports-copy") != std::string::npos,
          "launchd OOL COPY_SEND rights or transfer trace mismatch");
}

void check_in_job_port_identity_test() {
  AddressSpace launchd_memory;
  AddressSpace service_memory;
  constexpr std::uint32_t launchd_buffer = 0x44000U;
  constexpr std::uint32_t service_buffer = 0x45000U;
  require(
      launchd_memory.map(launchd_buffer, AddressSpace::page_size,
                         MemoryPermission::Read | MemoryPermission::Write) &&
          service_memory.map(service_buffer, AddressSpace::page_size,
                             MemoryPermission::Read | MemoryPermission::Write),
      "launchd check-in buffers failed to map");

  Dynarmic::ExclusiveMonitor launchd_monitor{1};
  Dynarmic::ExclusiveMonitor service_monitor{1};
  Cpu launchd_cpu{0, launchd_memory, launchd_monitor};
  Cpu service_cpu{0, service_memory, service_monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel launchd{launchd_memory, output};
  CompatibilityKernel service{service_memory, output};
  service.inherit_process_state(launchd, 2);

  const auto service_task_name =
      task_for_pid_name(launchd, launchd_cpu, launchd_memory,
                        launchd_buffer + 0x300U, service.process().pid);
  const auto launchd_job_name = allocate_receive_right(launchd, launchd_cpu);
  allocate_send_receive_right(launchd, launchd_cpu, launchd_memory,
                              launchd_buffer, launchd_job_name);
  set_bootstrap_special_port(launchd, launchd_cpu, launchd_memory,
                             launchd_buffer, service_task_name,
                             launchd_job_name);
  const auto service_job_name = get_bootstrap_special_port(
      service, service_cpu, service_memory, service_buffer);
  require(service_job_name != launchd_job_name,
          "job port copyout reused launchd's task-local name");

  const auto service_reply_name = allocate_receive_right(service, service_cpu);
  constexpr std::size_t collision_guard_count = 3;
  for (std::size_t index = 0; index < collision_guard_count; ++index) {
    static_cast<void>(allocate_receive_right(service, service_cpu));
  }

  launchd_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  launchd_cpu.registers()[0] = launchd_buffer;
  launchd_cpu.registers()[1] = mach_receive;
  launchd_cpu.registers()[2] = 0;
  launchd_cpu.registers()[3] = 256;
  launchd_cpu.registers()[4] = launchd_job_name;
  launchd.dispatch(launchd_cpu, 0x80);

  const auto check_in_id =
      xnu792::mig::bootstrap::id(xnu792::mig::bootstrap::Routine::check_in);
  require(
      service_memory.write32(
          service_buffer, darwin::mig_wire::message_bits(
                              darwin::mig_wire::disposition_copy_send,
                              darwin::mig_wire::disposition_make_send_once)) &&
          service_memory.write32(service_buffer + 4, 160) &&
          service_memory.write32(service_buffer + 8, service_job_name) &&
          service_memory.write32(service_buffer + 12, service_reply_name) &&
          service_memory.write32(service_buffer + 20, check_in_id),
      "bootstrap.check_in request setup failed");
  service_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  service_cpu.registers()[0] = service_buffer;
  service_cpu.registers()[1] = mach_send;
  service_cpu.registers()[2] = 160;
  service.dispatch(service_cpu, 0x80);
  require(service_cpu.registers()[0] == 0 &&
              launchd.deliver_pending_mach(launchd_cpu) &&
              launchd_memory.read32(launchd_buffer + 12) ==
                  std::optional<std::uint32_t>{launchd_job_name} &&
              launchd_memory.read32(launchd_buffer + 20) ==
                  std::optional<std::uint32_t>{check_in_id},
          "bootstrap.check_in lost launchd's local job-port identity");
  const auto launchd_reply_name =
      launchd_memory.read32(launchd_buffer + 8).value_or(0);
  require(launchd_reply_name != 0,
          "bootstrap.check_in reply right was not copied into launchd");

  const auto launchd_service_name =
      allocate_receive_right(launchd, launchd_cpu);
  allocate_send_receive_right(launchd, launchd_cpu, launchd_memory,
                              launchd_buffer, launchd_service_name);

  service_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  service_cpu.registers()[0] = service_buffer;
  service_cpu.registers()[1] = mach_receive;
  service_cpu.registers()[2] = 0;
  service_cpu.registers()[3] = 128;
  service_cpu.registers()[4] = service_reply_name;
  service.dispatch(service_cpu, 0x80);

  require(
      launchd_memory.write32(
          launchd_buffer,
          darwin::mig_wire::message_bits(
              darwin::mig_wire::disposition_move_send_once, 0, true)) &&
          launchd_memory.write32(launchd_buffer + 4, 40) &&
          launchd_memory.write32(launchd_buffer + 8, launchd_reply_name) &&
          launchd_memory.write32(launchd_buffer + 12, 0) &&
          launchd_memory.write32(launchd_buffer + 20, check_in_id + 100U) &&
          launchd_memory.write32(launchd_buffer + 24, 1) &&
          launchd_memory.write32(launchd_buffer + 28, launchd_service_name) &&
          launchd_memory.write32(
              launchd_buffer + 36,
              darwin::mig_wire::port_descriptor_metadata(
                  darwin::mig_wire::disposition_move_receive)),
      "bootstrap.check_in receive-right reply setup failed");
  launchd_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  launchd_cpu.registers()[0] = launchd_buffer;
  launchd_cpu.registers()[1] = mach_send;
  launchd_cpu.registers()[2] = 40;
  launchd.dispatch(launchd_cpu, 0x80);
  require(launchd_cpu.registers()[0] == 0 &&
              service.deliver_pending_mach(service_cpu),
          "bootstrap.check_in receive right was not delivered");

  const auto checked_in_name =
      service_memory.read32(service_buffer + 28).value_or(0);
  const auto send_mask = xnu792::ipc::type_mask(xnu792::ipc::Right::Send);
  const auto receive_mask = xnu792::ipc::type_mask(xnu792::ipc::Right::Receive);
  require(checked_in_name != 0 && checked_in_name != launchd_service_name &&
              port_type(launchd, launchd_cpu, launchd_memory, launchd_buffer,
                        launchd_service_name) == send_mask &&
              port_type(service, service_cpu, service_memory, service_buffer,
                        checked_in_name) == receive_mask,
          "bootstrap.check_in did not move the service receive right exactly");

  service_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  service_cpu.registers()[0] = service_buffer;
  service_cpu.registers()[1] = mach_receive;
  service_cpu.registers()[2] = 0;
  service_cpu.registers()[3] = 64;
  service_cpu.registers()[4] = checked_in_name;
  service.dispatch(service_cpu, 0x80);
  constexpr std::uint32_t service_message_id = 0x7801U;
  require(
      launchd_memory.write32(launchd_buffer,
                             darwin::mig_wire::message_bits(
                                 darwin::mig_wire::disposition_copy_send)) &&
          launchd_memory.write32(launchd_buffer + 4, 24) &&
          launchd_memory.write32(launchd_buffer + 8, launchd_service_name) &&
          launchd_memory.write32(launchd_buffer + 12, 0) &&
          launchd_memory.write32(launchd_buffer + 20, service_message_id),
      "checked-in service routing setup failed");
  launchd_cpu.registers()[12] = static_cast<std::uint32_t>(-31);
  launchd_cpu.registers()[0] = launchd_buffer;
  launchd_cpu.registers()[1] = mach_send;
  launchd_cpu.registers()[2] = 24;
  launchd.dispatch(launchd_cpu, 0x80);
  require(launchd_cpu.registers()[0] == 0 &&
              service.deliver_pending_mach(service_cpu) &&
              service_memory.read32(service_buffer + 12) ==
                  std::optional<std::uint32_t>{checked_in_name} &&
              service_memory.read32(service_buffer + 20) ==
                  std::optional<std::uint32_t>{service_message_id},
          "checked-in receive right did not retain its ipc_port identity");
}

} // namespace

void run_launchd_ipc_tests() {
  transfer_subset_ool_ports_test();
  check_in_job_port_identity_test();
}

} // namespace ilemu::test::mach_suite
