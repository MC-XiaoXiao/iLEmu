#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_proc_info_abi.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ilemu {

bool CompatibilityKernel::dispatch_bsd_process_information(
    Cpu &cpu, std::uint32_t number) {
  if (number != darwin::proc_info::syscall_number)
    return false;

  const auto &registers = cpu.registers();
  const auto call = registers[0];
  const auto target_pid = static_cast<std::int32_t>(registers[1]);
  const auto flavor = registers[2];
  const auto output_address = registers[5];
  const auto output_size = registers[6];

  if (call != darwin::proc_info::call_pid_info ||
      flavor != darwin::proc_info::flavor_pid_path_info) {
    bsd_error(cpu, darwin::error::invalid_argument);
    return true;
  }
  if (output_size < darwin::proc_info::path_info_size) {
    bsd_error(cpu, darwin::error::no_memory);
    return true;
  }
  if (output_size > darwin::proc_info::path_info_max_size) {
    bsd_error(cpu, darwin::error::value_too_large);
    return true;
  }

  std::string executable_path;
  {
    std::lock_guard lock{shared_state_->mach_mutex};
    const auto target = target_pid > 0
                            ? shared_state_->processes.find(
                                  static_cast<std::uint32_t>(target_pid))
                            : shared_state_->processes.end();
    if (target == shared_state_->processes.end() || target->second.exited ||
        target->second.executable_path.empty()) {
      bsd_error(cpu, darwin::error::no_such_process);
      return true;
    }
    executable_path = target->second.executable_path;
  }

  if (executable_path.size() >= output_size) {
    bsd_error(cpu, darwin::error::no_memory);
    return true;
  }
  std::vector<std::byte> output(output_size, std::byte{0});
  for (std::size_t index = 0; index < executable_path.size(); ++index) {
    output[index] = static_cast<std::byte>(
        static_cast<unsigned char>(executable_path[index]));
  }
  if (output_address == 0 || !memory_.copy_in(output_address, output)) {
    bsd_error(cpu, darwin::error::bad_address);
    return true;
  }

  output_.write("[process] pid-path caller=" +
                std::to_string(process_.pid) + " target=" +
                std::to_string(target_pid) + " path=" + executable_path +
                "\n");
  bsd_success(cpu, 0);
  return true;
}

} // namespace ilemu
