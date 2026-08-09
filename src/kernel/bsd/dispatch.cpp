#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_proc_info_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/kernel_bsd_interval_timer.hpp"
#include "ilemu/kernel_network.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {
namespace {

std::optional<std::uint32_t> canonical_no_cancel_syscall(
    std::uint32_t number) {
  // Darwin 9 adds non-cancellation syscall entry points for libSystem's
  // pthread cancellation boundary. The kernel operation is otherwise the same
  // as the cancellable BSD syscall, so reuse the existing compatibility path.
  switch (number) {
  case 396: // read_nocancel
    return darwin::syscall::read;
  case 397: // write_nocancel
    return darwin::syscall::write;
  case 398: // open_nocancel
    return darwin::syscall::open;
  case 399: // close_nocancel
    return darwin::syscall::close;
  case 400: // wait4_nocancel
    return 7U;
  case 401: // recvmsg_nocancel
    return darwin::syscall::receive_message;
  case 402: // sendmsg_nocancel
    return darwin::syscall::send_message;
  case 403: // recvfrom_nocancel
    return darwin::syscall::receive_from;
  case 404: // accept_nocancel
    return darwin::syscall::accept;
  case 406: // fcntl_nocancel
    return darwin::syscall::fcntl;
  case 407: // select_nocancel
    return darwin::syscall::select;
  case 408: // fsync_nocancel
    return darwin::syscall::synchronize_file;
  case 409: // connect_nocancel
    return darwin::syscall::connect;
  case 412: // writev_nocancel
    return darwin::syscall::write_vector;
  case 413: // sendto_nocancel
    return darwin::syscall::send_to;
  case 414: // pread_nocancel
    return 153U;
  case 415: // pwrite_nocancel
    return 154U;
  case 421: // aio_suspend_nocancel
    return darwin::syscall::aio_suspend;
  case 423: // __semwait_signal_nocancel
    return darwin::syscall::semaphore_wait_signal;
  default:
    return std::nullopt;
  }
}

} // namespace

void CompatibilityKernel::dispatch_bsd(Cpu &cpu, std::uint32_t number) {
  if (const auto canonical = canonical_no_cancel_syscall(number)) {
    dispatch_bsd(cpu, *canonical);
    return;
  }

  switch (number) {
  case 322: { // VersionSensitive nosys/iopolicysys collision.
    if (!darwin_abi_route_supported(
            legacy_iopolicysys_route,
            shared_state_->darwin_kernel_identity.capabilities.epoch)) {
      // xnu-792.24.17 and the firmware's pre-iopolicy xnu-933-era slot both
      // define syscall 322 as nosys. It must return ENOSYS without entering
      // trace_unknown(), because an expected nosys result is not a fatal ABI
      // violation.
      bsd_error(cpu, bsd_support::not_implemented);
      return;
    }

    constexpr std::uint32_t iopol_cmd_get = 1;
    constexpr std::uint32_t iopol_cmd_set = 2;
    constexpr std::uint32_t iopol_type_disk = 0;
    constexpr std::uint32_t iopol_scope_process = 0;
    constexpr std::uint32_t iopol_scope_thread = 1;
    constexpr std::uint32_t iopol_policy_max = 3;
    constexpr std::uint32_t iopol_policy_offset =
        2U * sizeof(std::uint32_t);

    const auto address = cpu.registers()[1];
    if (address > std::numeric_limits<std::uint32_t>::max() -
                     iopol_policy_offset) {
      bsd_error(cpu, bsd_support::bad_address);
      return;
    }
    const auto scope = memory_.read32(address);
    const auto iotype = memory_.read32(address + sizeof(std::uint32_t));
    const auto policy = memory_.read32(address + 2U * sizeof(std::uint32_t));
    if (!scope || !iotype || !policy) {
      bsd_error(cpu, bsd_support::bad_address);
      return;
    }
    if (*iotype != iopol_type_disk ||
        (*scope != iopol_scope_process && *scope != iopol_scope_thread)) {
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }

    auto *stored_policy = &process_.disk_io_policy;
    if (*scope == iopol_scope_thread) {
      const auto thread_object =
          thread_object_for_processor(cpu.processor_id());
      if (!thread_object) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      stored_policy = &process_.thread_disk_io_policies[*thread_object];
    }
    switch (cpu.registers()[0]) {
    case iopol_cmd_get:
      if (!memory_.write32(address + 2U * sizeof(std::uint32_t),
                           *stored_policy)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
      }
      bsd_success(cpu, 0);
      return;
    case iopol_cmd_set:
      if (*policy > iopol_policy_max) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
      }
      *stored_policy = *policy;
      bsd_success(cpu, 0);
      return;
    default:
      bsd_error(cpu, bsd_support::invalid_argument);
      return;
    }
  }
    return;
  case 0:
  case 1:
  case 2:
  case 66:
  case 7:
  case 20:
  case darwin::syscall::set_user_id:
  case 24:
  case 25:
  case 39:
  case 43:
  case 47:
  case 46:
  case 48:
  case 49:
  case 50:
  case 55:
  case 60:
  case 59:
  case darwin::syscall::get_process_group:
  case kernel_bsd::interval_timer::set_syscall:
  case kernel_bsd::interval_timer::get_syscall:
  case 244: // posix_spawn (xnu-1228 / iPhone OS user ABI)
  case 96:
  case 116:
  case darwin::syscall::set_time_of_day:
  case 147:
  case darwin::syscall::set_groups:
  case darwin::syscall::set_group_id:
  case darwin::syscall::set_effective_group_id:
  case darwin::syscall::set_effective_user_id:
  case darwin::syscall::init_groups:
  case darwin::syscall::get_resource_limit:
  case darwin::syscall::set_resource_limit:
  case darwin::syscall::disable_thread_signal:
  case 333:
  case darwin::syscall::semaphore_wait_signal:
  case darwin::syscall::semaphore_wait_signal_timespec:
  case darwin::proc_info::syscall_number:
  case 327:
  case 355:
    dispatch_bsd_process(cpu, number);
    return;
  case darwin::syscall::kill:
    dispatch_bsd_signal(cpu, number);
    return;
  case 9:
  case 10:
  case 5:
  case 6:
  case 12:
  case 13:
  case darwin::syscall::change_mode:
  case darwin::syscall::change_owner:
  case 18:
  case 33:
  case darwin::syscall::change_flags:
  case darwin::syscall::change_flags_fd:
  case darwin::syscall::change_owner_fd:
  case darwin::syscall::change_mode_fd:
  case darwin::syscall::change_mode_extended:
  case darwin::syscall::change_mode_extended_fd:
  case darwin::syscall::flock:
  case darwin::syscall::synchronize_file:
  case 36:
  case darwin::syscall::revoke:
  case 57:
  case 58:
  case 128:
  case 136:
  case 137:
  case darwin::syscall::update_file_times:
  case darwin::syscall::update_file_times_fd:
  case 153:
  case 154:
  case 157:
  case 159:
  case 167:
  case 158:
  case 201:
  case 196:
  case 199:
  case 220:
  case 221:
  case 344:
  case 338:
  case 339:
  case 340:
  case 341:
  case 342:
  case 343:
  case 345:
  case 346:
  case 347:
  case darwin::syscall::get_extended_attribute:
  case darwin::syscall::get_extended_attribute_fd:
  case darwin::syscall::set_extended_attribute:
  case darwin::syscall::set_extended_attribute_fd:
  case darwin::syscall::remove_extended_attribute:
  case darwin::syscall::remove_extended_attribute_fd:
  case darwin::syscall::list_extended_attributes:
  case darwin::syscall::list_extended_attributes_fd:
  case 188:
  case 190:
  case 189:
    dispatch_bsd_filesystem(cpu, number);
    return;
  case darwin::syscall::read:
  case darwin::syscall::write:
  case 41:
  case 42:
  case 73:
  case darwin::syscall::get_descriptor_table_size:
  case darwin::syscall::duplicate_to:
  case darwin::syscall::fcntl:
  case darwin::syscall::file_descriptor_path_configuration:
  case darwin::syscall::memory_protect:
  case darwin::syscall::memory_advise:
  case 197:
  case 266:
  case 267:
    dispatch_bsd_descriptor_memory(cpu, number);
    return;
  case 294:
  case 295:
  case 299:
  case 300:
    static_cast<void>(dispatch_bsd_shared_region(cpu, number));
    return;
  case darwin::syscall::aio_synchronize:
  case darwin::syscall::aio_return:
  case darwin::syscall::aio_suspend:
  case darwin::syscall::aio_cancel:
  case darwin::syscall::aio_error:
  case darwin::syscall::aio_read:
  case darwin::syscall::aio_write:
    dispatch_bsd_aio(cpu, number);
    return;
  case darwin::syscall::ptrace:
  case 180:
    static_cast<void>(dispatch_bsd_debug(cpu, number));
    return;
  case 27:
  case 28:
  case darwin::syscall::receive_from:
  case darwin::syscall::accept:
  case 31:
  case 32:
  case darwin::syscall::socket:
  case darwin::syscall::connect:
  case darwin::syscall::bind:
  case 105:
  case darwin::syscall::listen:
  case 118:
  case darwin::syscall::write_vector:
  case darwin::syscall::send_to:
  case darwin::syscall::shutdown:
  case darwin::syscall::socket_pair:
    dispatch_bsd_socket(cpu, number);
    return;
  case 54:
  case 93:
  case 202:
  case 362:
  case 363:
    dispatch_bsd_events(cpu, number);
    return;
  case darwin::syscall::mac_syscall:
    static_cast<void>(dispatch_bsd_security(cpu, number));
    return;
  default:
    trace_unknown(cpu, "BSD syscall", number);
    bsd_error(cpu, bsd_support::not_implemented);
    return;
  }
}

} // namespace ilemu
