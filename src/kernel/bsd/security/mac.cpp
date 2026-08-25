#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"

#include "../support.hpp"

#include <cstdint>
#include <string>

namespace ilemu {

bool CompatibilityKernel::dispatch_bsd_security(Cpu& cpu, std::uint32_t number)
{
    if (number != darwin::syscall::mac_syscall)
        return false;

    const auto& registers = cpu.registers();
    const auto policy = memory_.read_c_string(registers[0], 128U);
    if (!policy) {
        bsd_error(cpu, darwin::error::bad_address);
        return true;
    }

    // iPhone OS 2.x exposes the MAC policy syscall even when no emulated MAC
    // policy provider is installed. XNU's non-MAC build returns ENOSYS here;
    // that is safer than pretending a named policy operation succeeded.
    output_.write("[security] unsupported mac_syscall pid=" +
                  std::to_string(process_.pid) + " policy=" + *policy +
                  " call=" + std::to_string(registers[1]) + "\n");
    bsd_error(cpu, bsd_support::not_implemented); // ENOSYS
    return true;
}

} // namespace ilemu
