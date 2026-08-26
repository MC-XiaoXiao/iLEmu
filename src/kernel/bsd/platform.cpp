#include "ilemu/kernel.hpp"

#include "ilemu/content_identity.hpp"
#include "ilemu/darwin_abi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "support.hpp"

namespace ilemu {
namespace {

    constexpr std::size_t arm32_timespec_size = 8;
    constexpr std::size_t host_uuid_size = 16;

    std::array<std::byte, host_uuid_size> platform_uuid(
        const DeviceProfile& profile)
    {
        // A platform UUID identifies the emulated device, not the host that is
        // currently running it. Derive it from stable hardware-profile fields
        // so moving a data volume or changing the host graphics backend cannot
        // alter the guest-visible identity.
        std::string identity { "iLEmu-platform-uuid-v1" };
        for (const auto field : { profile.product_type, profile.board_config,
                 profile.model_number, profile.soc }) {
            identity.push_back('\0');
            identity.append(field);
        }

        const auto digest = sha256(
            std::as_bytes(std::span { identity.data(), identity.size() }));
        std::array<std::byte, host_uuid_size> uuid { };
        for (std::size_t index = 0; index < uuid.size(); ++index)
            uuid[index] = digest.digest[index];
        return uuid;
    }

} // namespace

void CompatibilityKernel::dispatch_bsd_platform(
    Cpu& cpu, std::uint32_t number)
{
    if (number != darwin::syscall::get_host_uuid) {
        trace_unknown(cpu, "BSD syscall", number);
        bsd_error(cpu, bsd_support::not_implemented);
        return;
    }

    const auto& registers = cpu.registers();
    // XNU copies in the caller's ARM32 timespec before asking IOKit for the
    // already-available platform UUID. Preserve that pointer-fault contract;
    // the timeout value itself does not delay a UUID that is immediately
    // available from the emulated platform expert.
    if (!memory_.read_bytes(registers[1], arm32_timespec_size)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }

    const auto uuid = platform_uuid(device_profile_);
    if (!memory_.copy_in(registers[0], uuid)) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }

    output_.write(
        "[platform] gethostuuid pid=" + std::to_string(process_.pid) + "\n");
    bsd_success(cpu, 0);
}

} // namespace ilemu
