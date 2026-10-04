// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Expose the loaded executable's signing status and entitlements via csops(2).
// https://github.com/apple-oss-distributions/xnu/blob/xnu-3248.20.55/bsd/kern/kern_proc.c

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"

#include <array>
#include <limits>
#include <mutex>
#include <span>
#include <vector>

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_code_signing(
    Cpu& cpu, bool require_audit_token)
{
    constexpr std::uint32_t entitlements_blob = 7U;
    constexpr std::uint32_t identity_blob = 11U;
    constexpr std::uint32_t entitlement_magic = 0xfade7171U;
    constexpr std::uint32_t header_size = 8U;
    const auto& registers = cpu.registers();
    const auto target_pid = registers[0] == 0U ? process_.pid : registers[0];
    const auto operation = registers[1];
    const auto address = registers[2];
    const auto capacity = registers[3];
    const auto audit_address = require_audit_token ? registers[4] : 0U;
    if (require_audit_token && audit_address == 0U) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    std::vector<std::byte> payload;
    {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto target = shared_state_->processes.find(target_pid);
        if (target == shared_state_->processes.end() || target->second.exited) {
            bsd_error(cpu, darwin::error::no_such_process);
            return;
        }
        if (require_audit_token) {
            constexpr std::uint32_t audit_token_size =
                8U * sizeof(std::uint32_t);
            if (!memory_.accessible(
                    audit_address, audit_token_size, MemoryPermission::Read)) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            const auto token_pid = memory_.read32(audit_address + 20U);
            const auto token_version = memory_.read32(audit_address + 28U);
            if (token_pid != target_pid ||
                token_version != target->second.audit_identity_version) {
                bsd_error(cpu, darwin::error::no_such_process);
                return;
            }
        }
        if (operation == 0U) { // CS_OPS_STATUS ignores usersize, as XNU does.
            if (address != 0U) {
                if (!memory_.accessible(address, sizeof(std::uint32_t),
                        MemoryPermission::Write)) {
                    bsd_error(cpu, darwin::error::bad_address);
                    return;
                }
                memory_.write32(address, target->second.code_signing_flags);
            }
            bsd_success(cpu, 0U);
            return;
        }
        if (operation == identity_blob) {
            if ((target->second.code_signing_flags & 0x10000001U) == 0U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            const auto& identity = target->second.code_signing_identity;
            if (identity.empty()) {
                bsd_error(cpu, darwin::error::no_entry);
                return;
            }
            const auto bytes =
                std::as_bytes(std::span { identity.c_str(), identity.size() + 1U });
            payload.assign(bytes.begin(), bytes.end());
        } else if (operation == entitlements_blob) {
            // Share the Mach-O payload already used by the AMFI user client.
            payload = target->second.code_signature_entitlements;
        } else {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
    }
    if (capacity < header_size) {
        bsd_error(cpu, darwin::error::result_too_large);
        return;
    }
    if (payload.size() >
        std::numeric_limits<std::uint32_t>::max() - header_size) {
        bsd_error(cpu, darwin::error::value_too_large);
        return;
    }
    const auto size = header_size + static_cast<std::uint32_t>(payload.size());
    const auto short_buffer = capacity < size;
    std::array<std::byte, header_size> header { };
    const auto encode = [&header](std::size_t offset, std::uint32_t value) {
        for (unsigned byte = 0; byte < 4U; ++byte)
            header[offset + byte] =
                static_cast<std::byte>(value >> ((3U - byte) * 8U));
    };
    // XNU returns eight zero bytes when no entitlements exist. Size probes
    // expose the required big-endian length; identity magic is always zero.
    if (!payload.empty()) {
        encode(0U, operation == identity_blob || short_buffer
                ? 0U : entitlement_magic);
        encode(4U, size);
    }
    std::vector<std::byte> blob { header.begin(), header.end() };
    if (!short_buffer)
        blob.insert(blob.end(), payload.begin(), payload.end());
    if (!memory_.copy_in(address, blob)) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    if (short_buffer) {
        bsd_error(cpu, darwin::error::result_too_large);
        return;
    }
    bsd_success(cpu, 0U);
}

} // namespace ilemu
