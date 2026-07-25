#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
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
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {

void CompatibilityKernel::dispatch_bsd(Cpu &cpu, std::uint32_t number) {
  switch (number) {
  case 0:
  case 1:
  case 2:
  case 66:
  case 7:
  case 20:
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
  case 244: // posix_spawn (xnu-1228 / iPhone OS user ABI)
  case 96:
  case 116:
  case darwin::syscall::set_time_of_day:
  case 147:
  case darwin::syscall::set_effective_group_id:
  case darwin::syscall::set_effective_user_id:
  case darwin::syscall::get_resource_limit:
  case darwin::syscall::set_resource_limit:
  case darwin::syscall::disable_thread_signal:
  case 333:
  case darwin::syscall::semaphore_wait_signal:
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
  case darwin::syscall::flock:
  case darwin::syscall::synchronize_file:
  case 36:
  case 57:
  case 58:
  case 128:
  case 136:
  case 137:
  case darwin::syscall::update_file_times:
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
  case darwin::syscall::memory_protect:
  case 197:
  case 266:
  case 267:
    dispatch_bsd_descriptor_memory(cpu, number);
    return;
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
  default:
    trace_unknown(cpu, "BSD syscall", number);
    bsd_error(cpu, bsd_support::not_implemented);
    return;
  }
}

} // namespace ilemu
