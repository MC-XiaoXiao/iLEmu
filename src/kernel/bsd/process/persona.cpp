/* SPDX-License-Identifier: MPL-2.0 */
// XNU3789 bsd/kern/sys_persona.c and bsd/sys/persona.h (ARM32).
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "../support.hpp"
#include <algorithm>

namespace ilemu {

std::uint32_t CompatibilityKernel::read_spawn_persona(
    std::uint32_t descriptor, SpawnAttributes& attributes)
{
    const auto size = memory_.read32(descriptor + 40U);
    const auto address = memory_.read32(descriptor + 44U);
    if (!size || !address)
        return darwin::error::bad_address;
    if (*size == 0U || *address == 0U)
        return 0U;
    if (*size != 88U)
        return darwin::error::result_too_large;
    std::array<std::uint32_t, 22> wire;
    if (!memory_.copy_out(*address, std::as_writable_bytes(std::span { wire })))
        return darwin::error::bad_address;
    if (process_.effective_uid != 0U)
        return darwin::error::operation_not_permitted;
    const std::lock_guard lock { shared_state_->mach_mutex };
    const auto info = shared_state_->personas.lookup(wire[0]);
    if (!info)
        return darwin::error::no_such_process;
    constexpr auto override_credentials = 1U, verify = 2U;
    constexpr auto set_uid = 0x10000U, set_gid = 0x20000U, set_groups = 0x40000U;
    const auto flags = wire[1], count = wire[4];
    if ((flags & (override_credentials | verify)) != 0U &&
        (flags & set_groups) != 0U && count > CredentialGroups::maximum_count)
        return darwin::error::invalid_argument;
    if ((flags & verify) != 0U) {
        if (((flags & set_uid) != 0U && wire[2] != info->id) ||
            ((flags & set_gid) != 0U && wire[3] != info->gid))
            return darwin::error::invalid_argument;
        if ((flags & set_groups) != 0U &&
            (count != info->group_count || wire[21] != info->membership_uid ||
                !std::equal(wire.begin() + 5U, wire.begin() + 5U + count, info->groups.begin())))
            return darwin::error::invalid_argument;
    }
    PersonaCredentials credentials { info, info->id, info->gid,
        info->gid, { }, info->name.data() };
    credentials.groups.assign(std::span { info->groups }.first(info->group_count));
    if ((flags & override_credentials) != 0U) {
        if ((flags & set_uid) != 0U)
            credentials.uid = wire[2];
        if ((flags & set_gid) != 0U) {
            credentials.groups.change_effective(credentials.effective_gid,
                wire[3], shared_state_->darwin_version);
            credentials.gid = credentials.effective_gid = wire[3];
        }
        if ((flags & set_groups) != 0U) {
            credentials.groups.assign(std::span { wire }.subspan(5U, count));
            if (count != 0U)
                credentials.effective_gid = wire[5];
        }
    }
    attributes.persona = std::move(credentials);
    return 0U;
}

void CompatibilityKernel::dispatch_bsd_persona(Cpu& cpu)
{
    const auto& r = cpu.registers();
    const auto operation = r[0], info_address = r[2], id_address = r[3];
    const std::lock_guard lock { shared_state_->mach_mutex };
    auto& registry = shared_state_->personas;
    const auto error = [&](std::uint32_t value) { bsd_error(cpu, value); };
    const auto copy_info = [&](const PersonaInfo& info) {
        const auto version = memory_.read32(info_address);
        if (!version) {
            error(darwin::error::bad_address);
            return false;
        }
        if (*version != 1U) {
            error(darwin::error::invalid_argument);
            return false;
        }
        if (!memory_.copy_to_user(info_address, std::as_bytes(std::span { &info, 1U }))) {
            error(darwin::error::bad_address);
            return false;
        }
        return true;
    };
    if ((operation == 1U || operation == 2U) && process_.effective_uid != 0U) {
        error(darwin::error::operation_not_permitted);
        return;
    }
    if (operation == 1U || operation == 6U) {
        const auto count = operation == 6U ? memory_.read32(r[4])
                                         : std::optional<std::uint32_t> { 0U };
        const auto version = memory_.read32(info_address);
        if (!count || !version) {
            error(darwin::error::bad_address);
            return;
        }
        if (*version != 1U) {
            error(darwin::error::invalid_argument);
            return;
        }
        PersonaInfo input;
        if (!memory_.copy_out(info_address,
                std::as_writable_bytes(std::span { &input, 1U }))) {
            error(darwin::error::bad_address);
            return;
        }
        input.name.back() = '\0';
        if (operation == 1U) {
            const auto allocated = registry.allocate(input);
            if (allocated.error != 0U) {
                error(allocated.error);
                return;
            }
            if (!memory_.write32(id_address, allocated.id)) {
                registry.remove(allocated.id);
                error(darwin::error::bad_address);
                return;
            }
            if (input.group_count != 0U && input.membership_uid == 0U)
                input.membership_uid = UINT32_MAX;
            if (!copy_info(input))
                return;
        } else {
            if (input.name[0] == '\0' && input.id == UINT32_MAX) {
                static_cast<void>(memory_.write32(r[4], 0U));
                error(darwin::error::invalid_argument);
                return;
            }
            const auto found = registry.find(input);
            for (std::size_t i = 0; i < std::min<std::size_t>(*count, found.size()); ++i) {
                if (!memory_.write32(id_address + static_cast<std::uint32_t>(i) * 4U, found[i])) {
                    static_cast<void>(memory_.write32(r[4], static_cast<std::uint32_t>(found.size())));
                    error(darwin::error::bad_address);
                    return;
                }
            }
            static_cast<void>(memory_.write32(r[4], static_cast<std::uint32_t>(found.size())));
            if (found.empty()) {
                error(darwin::error::no_such_process);
                return;
            }
        }
        bsd_success(cpu, 0U);
        return;
    }
    if (operation == 3U) {
        if (!process_.persona)
            error(darwin::error::no_such_process);
        else if (!memory_.write32(id_address, process_.persona->id))
            error(darwin::error::bad_address);
        else
            bsd_success(cpu, 0U);
        return;
    }
    if (operation == 2U || operation == 4U || operation == 5U) {
        const auto id = memory_.read32(id_address);
        if (!id) {
            error(darwin::error::bad_address);
            return;
        }
        auto identity = registry.lookup(*id);
        if (operation == 5U) {
            if (process_.effective_uid != 0U && *id != process_.pid) {
                error(darwin::error::operation_not_permitted);
                return;
            }
            const auto process = shared_state_->processes.find(*id);
            identity = process == shared_state_->processes.end()
                ? nullptr : process->second.persona;
        }
        if (operation == 2U) {
            if (!registry.remove(*id))
                error(darwin::error::no_such_process);
            else
                bsd_success(cpu, 0U);
            return;
        }
        if (!identity)
            error(darwin::error::no_such_process);
        else if (copy_info(*identity))
            bsd_success(cpu, 0U);
        return;
    }
    error(bsd_support::not_implemented);
}

} // namespace ilemu
