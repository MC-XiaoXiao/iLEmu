/* SPDX-License-Identifier: MPL-2.0 */
// Compact ARM32 nxctl socket-option records, as used by libsystem_kernel.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"

#include <algorithm>

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_nexus_options(
    Cpu& cpu, nexus::Controller& controller, bool set)
{
    const auto& r = cpu.registers();
    const auto option = r[1], address = r[2];
    std::uint32_t capacity { r[3] };
    if (!set) {
        if (r[3] == 0U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        if (address != 0U) {
            const auto size = memory_.read32(r[3]);
            if (!size) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            capacity = *size;
        } else {
            capacity = 0U;
        }
    }
    const auto wire_size = set
        ? (option == 21U ? 52U : option == 22U ? 20U : option == 40U ? 28U : 0U)
        : (option == 1U ? 8U : option == 2U ? 132U :
            (option == 20U || option == 30U) ? 24U : 0U);
    if (wire_size == 0U) {
        bsd_error(cpu, darwin::error::no_protocol_option);
        return;
    }
    if (address == 0U || capacity < wire_size) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    std::array<std::uint32_t, 33> words { };
    auto wire = std::as_writable_bytes(std::span { words }).first(wire_size);
    if (!memory_.copy_out(address, wire)) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    nexus::Identifier identifier { };
    if (option != 1U) {
        std::copy_n(wire.begin(), identifier.size(), identifier.begin());
        if (identifier == nexus::Identifier { }) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
    }
    std::uint32_t error { };
    if (set) {
        if (option == 21U) {
            nexus::PortBinding binding;
            binding.flags = words[5] & 7U;
            binding.process = words[12];
            std::copy_n(wire.begin() + 24U, identifier.size(), binding.executable.begin());
            if (binding.flags == 0U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            if ((binding.flags & 1U) != 0U || ((binding.flags & 2U) != 0U &&
                    binding.executable == nexus::Identifier { })) {
                const std::lock_guard lock { shared_state_->mach_mutex };
                const auto process = shared_state_->processes.find(binding.process);
                if (process == shared_state_->processes.end() || process->second.exited) {
                    bsd_error(cpu, darwin::error::no_such_process);
                    return;
                }
                if (binding.process == 0U) {
                    bsd_error(cpu, darwin::error::permission_denied);
                    return;
                }
                binding.process_incarnation = process->second.incarnation;
                binding.executable = process->second.executable_uuid;
            }
            if ((binding.flags & 4U) != 0U) {
                // Native NEXUS_MAX_KEY_LEN: the target checks (length-1)>>10.
                if (words[10] == 0U || words[11] == 0U || words[11] > 1024U) {
                    bsd_error(cpu, darwin::error::invalid_argument);
                    return;
                }
                auto key = memory_.read_bytes(words[10], words[11]);
                if (!key) {
                    bsd_error(cpu, darwin::error::bad_address);
                    return;
                }
                binding.key = std::move(*key);
            }
            error = controller.bind(identifier, words[4], std::move(binding));
        } else if (option == 22U) {
            error = controller.unbind(identifier, words[4]);
        } else {
            error = controller.configure(identifier); // User pipe has no config callback.
        }
    } else if (option == 2U) {
        nexus::ProviderParameters parameters;
        error = controller.provider_parameters(identifier, parameters);
        if (error == 0U)
            std::copy(parameters.begin(), parameters.end(), wire.begin() + identifier.size());
    } else if (option == 1U) {
        const auto entries = controller.providers();
        const auto count = std::min<std::size_t>(words[0], entries.size());
        if (words[1] != 0U && count != 0U &&
            !memory_.copy_to_user(words[1], std::as_bytes(std::span { entries }.first(count))))
            error = darwin::error::bad_address;
        words[0] = static_cast<std::uint32_t>(entries.size());
    } else {
        std::vector<nexus::Identifier> identifiers;
        if (option == 20U)
            error = controller.instances(identifier, identifiers);
        else
            error = controller.channel_identifiers(identifier, identifiers);
        if (error == 0U) {
            const auto count_word = 4U;
            const auto count = std::min<std::size_t>(words[count_word], identifiers.size());
            const auto destination = words[count_word + 1U];
            if (destination != 0U && count != 0U) {
                if (!memory_.copy_to_user(destination,
                        std::as_bytes(std::span { identifiers }.first(count)))) {
                    error = darwin::error::bad_address;
                }
            }
            words[count_word] = static_cast<std::uint32_t>(identifiers.size());
        }
    }
    if (!set && error == 0U &&
        (!memory_.copy_to_user(address, wire) || !memory_.write32(r[3], wire_size)))
        error = darwin::error::bad_address;
    if (error != 0U)
        bsd_error(cpu, error);
    else
        bsd_success(cpu, 0U);
}

} // namespace ilemu
