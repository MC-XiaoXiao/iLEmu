/* SPDX-License-Identifier: MPL-2.0 */
// XNU skywalk/nexus/nexus_syscalls.c. Compact ARM32 provider registration and
// controller-init ABI are confirmed by the firmware's libsystem_kernel wrappers.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "crypto/host_uuid.hpp"
#include "foundation/code_signature.hpp"

#include <algorithm>
#include <functional>

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_nexus(Cpu& cpu, std::uint32_t number)
{
    const auto& registers = cpu.registers();
    if (number == 503U) {
        const auto address = registers[0];
        if (address == 0U || registers[1] < 16U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        auto initialization = memory_.read_bytes(address, 16U);
        if (!initialization) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        if (memory_.read32(address) != 1U) {
            bsd_error(cpu, darwin::error::not_supported);
            return;
        }
        const auto fd = allocate_file_descriptor();
        if (!fd) {
            bsd_error(cpu, darwin::error::too_many_open_files);
            return;
        }
        const auto guard = static_cast<std::uint64_t>(std::hash<std::string> { }(make_host_uuid()));
        for (unsigned i = 0; i < 8U; ++i)
            (*initialization)[8U + i] = static_cast<std::byte>(guard >> (i * 8U));
        if (!memory_.copy_to_user(address, *initialization)) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        std::array<bool, 5> privileges;
        {
            const std::lock_guard lock { shared_state_->mach_mutex };
            const auto& entitlements =
                shared_state_->processes.at(process_.pid).code_signature_entitlements;
            constexpr std::array entitlement_names {
                "com.apple.private.skywalk.register-user-pipe",
                "com.apple.private.skywalk.register-kernel-pipe",
                "com.apple.private.skywalk.register-net-if",
                "com.apple.private.skywalk.register-flow-switch",
                "com.apple.private.skywalk.observe-all"
            };
            for (std::size_t i = 0; i < privileges.size(); ++i)
                privileges[i] = process_.effective_uid == 0U ||
                    CodeSignature::has_boolean_entitlement(entitlements, entitlement_names[i]);
        }
        nexus_controllers_[*fd] = shared_state_->nexus_registry.open(privileges);
        virtual_descriptors_[*fd] = "nexus";
        descriptor_guards_.emplace(*fd,
            DarwinFileGuard { guard, DarwinFileGuard::supported_flags });
        descriptor_flags_[*fd] = DarwinFileGuard::descriptor_close_on_exec |
            DarwinFileGuard::descriptor_close_on_fork;
        file_status_flags_[*fd] = darwin::open_flag::read_write;
        bsd_success(cpu, *fd);
        return;
    }

    std::optional<std::vector<std::byte>> registration;
    nexus::Identifier identifier;
    if (number == 504U) {
        if (registers[1] == 0U || registers[2] < nexus::CompactProviderRegistration::size ||
            registers[3] == 0U || registers[4] < identifier.size()) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        registration = memory_.read_bytes(registers[1], nexus::CompactProviderRegistration::size);
        if (!registration) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        if (const auto error = nexus::CompactProviderRegistration::validate(*registration)) {
            bsd_error(cpu, error);
            return;
        }
    } else if (number >= 505U && number <= 507U) {
        if (registers[2] < identifier.size() || (number == 506U &&
                (registers[3] == 0U || registers[4] < identifier.size()))) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        const auto bytes = memory_.read_bytes(registers[1], identifier.size());
        if (!bytes) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        std::copy(bytes->begin(), bytes->end(), identifier.begin());
        if (identifier == nexus::Identifier { }) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
    }
    const auto controller = nexus_controllers_.find(registers[0]);
    if (controller == nexus_controllers_.end()) {
        bsd_error(cpu, descriptor_valid(registers[0]) ? 19U : darwin::error::bad_file_descriptor);
        return; // ENODEV for a descriptor of another type.
    }
    if (number == 508U || number == 509U) {
        dispatch_bsd_nexus_options(cpu, *controller->second, number == 509U);
        return;
    }
    std::uint32_t error { };
    if (number == 504U) {
        error = controller->second->register_provider(*registration, identifier);
        if (error == 0U && !memory_.copy_to_user(registers[3], identifier)) {
            static_cast<void>(controller->second->deregister_provider(identifier));
            error = darwin::error::bad_address;
        }
    } else if (number == 505U) {
        error = controller->second->deregister_provider(identifier);
    } else if (number == 506U) {
        nexus::Identifier instance;
        error = controller->second->create_instance(identifier, instance);
        if (error == 0U && !memory_.copy_to_user(registers[3], instance)) {
            static_cast<void>(controller->second->destroy_instance(instance));
            error = darwin::error::bad_address;
        }
    } else {
        error = controller->second->destroy_instance(identifier);
    }
    if (error != 0U)
        bsd_error(cpu, error);
    else
        bsd_success(cpu, 0U);
}

} // namespace ilemu
