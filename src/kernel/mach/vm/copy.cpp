#include "ilemu/kernel.hpp"

#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/vm_map_mig_ids.hpp"

#include <cstdint>
#include <string>

#include "../support.hpp"
#include "wire_reply.hpp"

namespace ilemu {
namespace {

constexpr std::uint32_t vm_copy_request_size =
    xnu792::mig::vm_map::vm_copy_arguments[3].request_offset +
    darwin::mig_wire::word_size;

} // namespace

bool CompatibilityKernel::dispatch_mach_vm_copy_message(
    Cpu &cpu, const MachMessageRequest &request) {
  using xnu792::mig::vm_map::Routine;
  using namespace mach_support;
  using namespace mach_vm_support;

  if (request.identifier != mig_message_id(Routine::vm_copy))
    return false;

  auto &registers = cpu.registers();
  const auto fail_transport = [&] {
    registers[0] = mach_receive_invalid_data;
    return true;
  };
  const auto finish = [&](std::uint32_t result) {
    if (!write_simple_reply(memory_, request.address, request.local_port,
                            request.identifier, result)) {
      return fail_transport();
    }
    registers[0] = kern_success;
    return true;
  };

  if (registers[2] < vm_copy_request_size || registers[3] < simple_reply_size) {
    return fail_transport();
  }

  const auto &arguments = xnu792::mig::vm_map::vm_copy_arguments;
  const auto source =
      memory_.read32(request.address + arguments[1].request_offset);
  const auto size =
      memory_.read32(request.address + arguments[2].request_offset);
  const auto destination =
      memory_.read32(request.address + arguments[3].request_offset);
  if (!source || !size || !destination)
    return fail_transport();

  if (*size > maximum_message_io)
    return finish(kern_resource_shortage);

  const auto bytes = memory_.read_bytes(*source, *size);
  if (!bytes ||
      !memory_.accessible(*destination, *size, MemoryPermission::Write)) {
    return finish(kern_invalid_address);
  }

  // Read into an independent buffer before overwriting the destination.
  // This preserves XNU vm_map_copyin/vm_map_copy_overwrite overlap semantics.
  if (!memory_.copy_in(*destination, *bytes))
    return finish(kern_invalid_address);

  output_.write("[vm] copy pid=" + std::to_string(process_.pid) +
                " source=" + std::to_string(*source) +
                " destination=" + std::to_string(*destination) +
                " size=" + std::to_string(*size) + " result=0\n");
  return finish(kern_success);
}

} // namespace ilemu
