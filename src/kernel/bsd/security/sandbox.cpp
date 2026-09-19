// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Implement the guest sandbox compatibility query boundary.

#include "sandbox.hpp"

#include "foundation/address_space.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace ilemu::bsd::sandbox {
namespace {

    constexpr std::uint32_t initialize_compiled_profile = 0U;
    constexpr std::uint32_t initialize_named_profile = 1U;
    constexpr std::uint32_t check_operation = 2U;
    constexpr std::uint32_t path_filter = 1U;
    constexpr std::size_t request_word_count = 6U;
    constexpr std::size_t maximum_operation_size = 256U;
    constexpr std::size_t maximum_path_size = 4096U;

} // namespace

CallResult dispatch(
    AddressSpace& memory, DarwinSandboxAbi abi, std::uint32_t operation,
    std::uint32_t argument)
{
    if (operation != check_operation && operation != initialize_named_profile &&
        operation != initialize_compiled_profile)
        return CallResult::Unsupported;
    const auto slot_size =
        abi == DarwinSandboxAbi::Wide64Arguments ? 8U : 4U;
    const auto slot_count = operation == initialize_named_profile ? 1U
        : operation == initialize_compiled_profile ? 2U : request_word_count;
    const auto request_size = static_cast<std::uint32_t>(slot_count) * slot_size;
    if (argument == 0U ||
        argument >
            std::numeric_limits<std::uint32_t>::max() -
                (request_size - 1U)) {
        return CallResult::BadAddress;
    }

    std::array<std::uint64_t, request_word_count> request { };
    for (std::size_t index = 0; index < slot_count; ++index) {
        const auto address = argument + static_cast<std::uint32_t>(index) * slot_size;
        const std::optional<std::uint64_t> value = slot_size == 8U
            ? memory.read64(address)
            : std::optional<std::uint64_t> { memory.read32(address) };
        if (!value)
            return CallResult::BadAddress;
        request[index] = *value;
    }

    if (operation == initialize_compiled_profile) {
        // The compiled declaration carries a profile pointer and byte length
        // in ABI-sized slots. Validate the complete guest buffer without
        // allocating a host copy or installing a host policy. Like named
        // initialization below, this belongs to the existing non-enforcing
        // compatibility provider; it is not a compiled-policy interpreter.
        const auto profile_address = request[0];
        const auto profile_size = request[1];
        if (profile_size == 0U)
            return CallResult::InvalidArgument;
        if (profile_address == 0U || profile_address > UINT32_MAX ||
            profile_size > UINT32_MAX ||
            profile_size > (std::uint64_t { 1 } << 32U) - profile_address ||
            !memory.accessible(static_cast<std::uint32_t>(profile_address),
                static_cast<std::size_t>(profile_size), MemoryPermission::Read))
            return CallResult::BadAddress;
        return CallResult::Success;
    }

    if (operation == initialize_named_profile) {
        if (request[0] == 0U || request[0] > UINT32_MAX)
            return CallResult::BadAddress;
        const auto name = memory.read_c_string(
            static_cast<std::uint32_t>(request[0]), maximum_path_size);
        if (!name)
            return CallResult::BadAddress;
        if (name->empty())
            return CallResult::InvalidArgument;
        // Named-profile initialization uses the same non-enforcing guest
        // provider as checks below. No host sandbox policy is installed.
        return CallResult::Success;
    }

    if (request[0] == 0U || request[0] > UINT32_MAX ||
        request[2] == 0U || request[2] > UINT32_MAX ||
        (request[3] == path_filter && request[4] > UINT32_MAX))
        return CallResult::BadAddress;
    const auto decision_address = static_cast<std::uint32_t>(request[0]);
    const auto operation_name_address = static_cast<std::uint32_t>(request[2]);
    const auto filter_kind = request[3];
    const auto filter_address = static_cast<std::uint32_t>(request[4]);
    if (!memory.read_c_string(operation_name_address, maximum_operation_size))
        return CallResult::BadAddress;
    if (filter_kind == path_filter &&
        !memory.read_c_string(filter_address, maximum_path_size)) {
        return CallResult::BadAddress;
    }

    // The compatibility kernel does not install or enforce a Sandbox MAC
    // policy. A check therefore has no policy denial to report. Returning
    // ENOSYS here makes libsandbox treat that absence as a denied operation.
    constexpr std::array<std::uint32_t, 2> allowed { 0U, 0U };
    if (!memory.copy_in(decision_address, std::as_bytes(std::span { allowed })))
        return CallResult::BadAddress;
    return CallResult::Success;
}

} // namespace ilemu::bsd::sandbox
