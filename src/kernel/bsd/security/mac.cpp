#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"

#include "../support.hpp"
#include "sandbox.hpp"

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

    if (*policy == "Sandbox") {
        switch (bsd::sandbox::dispatch(
            memory_, registers[1], registers[2])) {
        case bsd::sandbox::CallResult::Success:
            bsd_success(cpu, 0);
            return true;
        case bsd::sandbox::CallResult::BadAddress:
            bsd_error(cpu, darwin::error::bad_address);
            return true;
        case bsd::sandbox::CallResult::Unsupported:
            break;
        }
    }

    // Preserve ENOSYS for policy operations whose ABI or state is not
    // represented by an emulated provider.
    output_.write("[security] unsupported mac_syscall pid=" +
                  std::to_string(process_.pid) + " policy=" + *policy +
                  " call=" + std::to_string(registers[1]) + "\n");
    bsd_error(cpu, bsd_support::not_implemented); // ENOSYS
    return true;
}

} // namespace ilemu
