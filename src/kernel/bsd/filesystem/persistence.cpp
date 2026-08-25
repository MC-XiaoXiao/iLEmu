#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"

#include <cerrno>
#include <string>
#include <system_error>

#include <unistd.h>

#include "../support.hpp"

namespace ilemu {

bool CompatibilityKernel::dispatch_bsd_filesystem_persistence(
    Cpu& cpu, std::uint32_t number)
{
    if (number != darwin::syscall::synchronize_file)
        return false;

    auto fd = cpu.registers()[0];
    if (const auto duplicate = duplicated_descriptors_.find(fd);
        duplicate != duplicated_descriptors_.end()) {
        fd = duplicate->second;
    }
    const auto descriptor = file_descriptors_.find(fd);
    if (descriptor == file_descriptors_.end()) {
        bsd_error(cpu, bsd_support::bad_file_descriptor);
        return true;
    }

    const auto description = ensure_regular_file_open_description(fd);
    if (!description) {
        bsd_error(cpu, bsd_support::bad_file_descriptor);
        return true;
    }
    const auto result = ::fsync(description->host_descriptor());
    const auto sync_error = errno;
    if (result != 0) {
        bsd_error(cpu, bsd_support::darwin_filesystem_error(std::error_code {
                           sync_error, std::generic_category() }));
        return true;
    }
    output_.write("[vfs] fsync fd=" + std::to_string(fd) + "\n");
    bsd_success(cpu, 0);
    return true;
}

} // namespace ilemu
