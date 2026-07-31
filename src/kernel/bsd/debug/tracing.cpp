#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"

#include "../support.hpp"

#include <cstdint>
#include <string>

namespace ilemu {

bool CompatibilityKernel::dispatch_bsd_debug(Cpu &cpu,
                                              std::uint32_t number) {
  if (number == darwin::syscall::ptrace) {
    const auto request = cpu.registers()[0];
    const auto target_pid = cpu.registers()[1];

    if (request == darwin::ptrace_request::deny_attach) {
      // PT_DENY_ATTACH marks the current process as refusing future debugger
      // attachment. The emulator has no ptrace debugger attachment model, and
      // the current process is never marked traced, so the successful Darwin
      // path is the observable behavior guest anti-debug calls need.
      output_.write("[debug] ptrace PT_DENY_ATTACH pid=" +
                    std::to_string(process_.pid) + "\n");
      bsd_success(cpu, 0);
      return true;
    }

    if (request == darwin::ptrace_request::attach && target_pid < 2) {
      bsd_error(cpu, darwin::error::operation_not_permitted);
      return true;
    }

    output_.write("[debug] unsupported ptrace request=" +
                  std::to_string(request) +
                  " target=" + std::to_string(target_pid) + "\n");
    bsd_error(cpu, 45); // ENOTSUP
    return true;
  }

  if (number != 180)
    return false;

  // xnu-792 bsd/kern/kdebug.c returns EINVAL while kernel tracing is
  // disabled.  SpringBoard emits animation signposts through this syscall;
  // the unavailable trace sink must not stop the calling guest thread.
  bsd_error(cpu, bsd_support::invalid_argument);
  return true;
}

} // namespace ilemu
