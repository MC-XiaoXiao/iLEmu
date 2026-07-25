#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"

#include <cstdint>
#include <string>

namespace ilemu {

bool CompatibilityKernel::dispatch_bsd_process_credentials(
    Cpu &cpu, std::uint32_t number) {
  if (number != darwin::syscall::set_effective_group_id &&
      number != darwin::syscall::set_effective_user_id)
    return false;

  const auto requested_group = cpu.registers()[0];
  if (number == darwin::syscall::set_effective_user_id) {
    const auto requested_user = cpu.registers()[0];
    // As with the saved group below, the saved user currently equals the
    // real user because set-id executable transitions are not modeled yet.
    if (process_.effective_uid != 0 && requested_user != process_.uid) {
      bsd_error(cpu, darwin::error::operation_not_permitted);
      return true;
    }
    process_.effective_uid = requested_user;
    if (const auto record = shared_state_->processes.find(process_.pid);
        record != shared_state_->processes.end()) {
      record->second.effective_uid = requested_user;
    }
    output_.write("[process] seteuid pid=" + std::to_string(process_.pid) +
                  " uid=" + std::to_string(requested_user) + "\n");
    bsd_success(cpu, 0);
    return true;
  }

  // XNU 792 permits the real or saved group without privilege. The current
  // loader has no set-id image transition yet, so the saved group equals the
  // real group. Root may select any effective group.
  if (process_.effective_uid != 0 && requested_group != process_.gid) {
    bsd_error(cpu, darwin::error::operation_not_permitted);
    return true;
  }

  process_.effective_gid = requested_group;
  if (const auto record = shared_state_->processes.find(process_.pid);
      record != shared_state_->processes.end()) {
    record->second.effective_gid = requested_group;
  }
  output_.write("[process] setegid pid=" + std::to_string(process_.pid) +
                " gid=" + std::to_string(requested_group) + "\n");
  bsd_success(cpu, 0);
  return true;
}

} // namespace ilemu
