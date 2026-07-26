#include "ilemu/kernel.hpp"
#include "ilemu/performance.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "../support.hpp"

namespace ilemu {
namespace {

constexpr std::uint32_t posix_spawn_syscall = 244U;
constexpr std::uint16_t posix_spawn_start_suspended = 0x0080U;
constexpr std::uint32_t maximum_vector_entries = 4096U;
constexpr std::uint32_t maximum_string_size = 64U * 1024U;

std::optional<std::vector<std::string>>
read_string_vector(const AddressSpace &memory, std::uint32_t address) {
  std::vector<std::string> values;
  if (address == 0)
    return values;
  for (std::uint32_t index = 0; index < maximum_vector_entries; ++index) {
    const auto pointer = memory.read32(address + index * 4U);
    if (!pointer)
      return std::nullopt;
    if (*pointer == 0)
      return values;
    const auto value = memory.read_c_string(*pointer, maximum_string_size);
    if (!value)
      return std::nullopt;
    values.push_back(*value);
  }
  return std::nullopt;
}

} // namespace

bool CompatibilityKernel::dispatch_bsd_process_spawn(Cpu &cpu,
                                                      std::uint32_t number) {
  if (number != posix_spawn_syscall)
    return false;

  PerformanceLatencyScope total_latency{PerfLatencyKind::PosixSpawnTotal};
  auto &registers = cpu.registers();
  const auto pid_address = registers[0];
  std::optional<std::string> path;
  std::optional<std::vector<std::string>> arguments;
  std::optional<std::vector<std::string>> environment;
  {
    PerformanceLatencyScope decode_latency{PerfLatencyKind::PosixSpawnDecode};
    path = memory_.read_c_string(registers[1]);
    arguments = read_string_vector(memory_, registers[3]);
    environment = read_string_vector(memory_, registers[4]);
  }
  if (pid_address == 0 || !path || !arguments || !environment) {
    bsd_error(cpu, bsd_support::bad_address);
    return true;
  }

  bool start_suspended = false;
  if (registers[2] != 0) {
    const auto attribute_size = memory_.read32(registers[2]);
    const auto attribute_address = memory_.read32(registers[2] + 4U);
    if (!attribute_size || !attribute_address) {
      bsd_error(cpu, bsd_support::bad_address);
      return true;
    }
    if (*attribute_size >= sizeof(std::uint16_t) && *attribute_address != 0) {
      const auto flags = memory_.read16(*attribute_address);
      if (!flags) {
        bsd_error(cpu, bsd_support::bad_address);
        return true;
      }
      start_suspended = (*flags & posix_spawn_start_suspended) != 0;
    }
  }
  // `--suspended` is an application argument used by first-generation
  // SpringBoard's prewarm protocol, not a kernel spawn attribute. The child
  // must run through dyld and suspend itself in userspace. Only the actual
  // POSIX_SPAWN_START_SUSPENDED flag places the initial thread on hold.

  std::error_code path_error;
  if (!std::filesystem::is_regular_file(resolve_guest_path(*path),
                                        path_error)) {
    bsd_error(cpu, 2); // ENOENT
    return true;
  }

  std::optional<std::uint32_t> child;
  if (spawn_create_handler_) {
    PerformanceLatencyScope create_latency{
        PerfLatencyKind::PosixSpawnCreate};
    child = spawn_create_handler_(cpu);
  } else {
    PerformanceLatencyScope fork_latency{PerfLatencyKind::PosixSpawnFork};
    child = fork_handler_ ? fork_handler_(cpu) : std::nullopt;
  }
  if (!child) {
    bsd_error(cpu, 11); // EAGAIN
    return true;
  }
  performance_counters().record_fork();
  if (!spawn_exec_handler_ ||
      !spawn_exec_handler_(*child, *path, *arguments, *environment,
                           start_suspended)) {
    bsd_error(cpu, 8); // ENOEXEC
    return true;
  }
  performance_counters().record_exec();
  if (!memory_.write32(pid_address, *child)) {
    bsd_error(cpu, bsd_support::bad_address);
    return true;
  }

  std::ostringstream message;
  message << "[process] spawn parent=" << process_.pid << " child=" << *child
          << " suspended=" << start_suspended << " " << *path << " argv=";
  for (std::size_t index = 0; index < arguments->size(); ++index) {
    if (index != 0)
      message << ',';
    message << '"' << (*arguments)[index] << '"';
  }
  message << '\n';
  output_.write(message.str());
  bsd_success(cpu, 0);
  return true;
}

} // namespace ilemu
