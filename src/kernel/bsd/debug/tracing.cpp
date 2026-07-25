#include "ilemu/kernel.hpp"

#include "../support.hpp"

#include <cstdint>

namespace ilemu {

bool CompatibilityKernel::dispatch_bsd_debug(Cpu &cpu,
                                              std::uint32_t number) {
  if (number != 180) return false;

  // xnu-792 bsd/kern/kdebug.c returns EINVAL while kernel tracing is
  // disabled.  SpringBoard emits animation signposts through this syscall;
  // the unavailable trace sink must not stop the calling guest thread.
  bsd_error(cpu, bsd_support::invalid_argument);
  return true;
}

} // namespace ilemu
