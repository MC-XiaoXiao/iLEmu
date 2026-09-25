// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "device_client.hpp"

#include "foundation/address_space.hpp"
#include "kernel/iokit_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "../../mach/support.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

namespace ilemu::kernel_iokit::graphics {
namespace {
    // Internal mapping key, not an IOConnectMapMemory memory selector.
    constexpr auto device_mapping_key =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint32_t mapping_reply_size = 0x260U;
    constexpr std::uint32_t config_reply_size = 32U;
    constexpr std::uint32_t name_reply_size = 64U;
    constexpr std::uint32_t arena_size = 2U * AddressSpace::page_size;
    constexpr std::uint64_t maximum_memory_bytes = 512ULL * 1024U * 1024U;

    void word(std::vector<std::byte>& bytes, std::size_t offset,
        std::uint32_t value)
    {
        for (std::size_t index = 0; index < sizeof(value); ++index)
            bytes[offset + index] =
                static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }

    void string(std::vector<std::byte>& bytes, std::size_t offset,
        std::string_view value)
    {
        std::transform(value.begin(), value.end(), bytes.begin() + offset,
            [](char c) { return static_cast<std::byte>(c); });
    }
} // namespace

std::optional<MethodResult> dispatch_device_method_locked(AddressSpace& memory,
    KernelSharedState& state, std::uint32_t connection_object,
    std::uint32_t selector, std::span<const std::uint64_t> scalar_input,
    std::span<const std::byte> inband_input,
    std::uint32_t scalar_output_capacity,
    std::uint32_t inband_output_capacity)
{
    const auto client = state.iokit_connections.find(connection_object);
    if (client == state.iokit_connections.end() || client->second.type != 1U)
        return std::nullopt;

    auto& connection = state.iokit_graphics_connections[connection_object];
    auto mapping = connection.memory_mappings.find(device_mapping_key);
    const bool initialized = mapping != connection.memory_mappings.end();
    const bool mapping_request =
        selector == 2U && inband_output_capacity == mapping_reply_size;
    if (!initialized && !mapping_request)
        return std::nullopt;
    if (!scalar_input.empty() || !inband_input.empty() ||
        scalar_output_capacity != 0U)
        return MethodResult { iokit_abi::bad_argument, {}, {} };

    std::vector<std::byte> result;
    switch (selector) {
    case 2U:
        if (inband_output_capacity != mapping_reply_size)
            return MethodResult { iokit_abi::bad_argument, {}, {} };
        if (!initialized) {
            const auto address = mach_support::find_free_guest_region(
                memory, 0x1d000000U, arena_size);
            if (!address || !memory.map(*address, arena_size,
                    MemoryPermission::Read | MemoryPermission::Write))
                return MethodResult { iokit_abi::no_memory, {}, {} };
            mapping = connection.memory_mappings.emplace(device_mapping_key,
                KernelSharedState::IOKitGraphicsConnectionState::MemoryMapping {
                    *address, arena_size, arena_size }).first;
        }
        result.resize(mapping_reply_size);
        // Native IOAccelDeviceCreate consumes two 64-bit guest addresses,
        // a queue count, and the firmware event predicate's symbol name.
        // The host GL dispatch backend does not submit kernel GPU commands.
        // This is one initially idle event queue, with no active resources;
        // never mark an outstanding event complete to bypass a wait.
        word(result, 0U, mapping->second.address);
        word(result, 8U, mapping->second.address + AddressSpace::page_size);
        word(result, 0x1cU, 1U);
        string(result, 0x20U, "IOAccelDeviceTestEventBasic");
        break;
    case 0U: {
        if (inband_output_capacity != config_reply_size)
            return MethodResult { iokit_abi::bad_argument, {}, {} };
        result.resize(config_reply_size);
        // Optional device feature bits remain clear. GetConfig64 exposes
        // memory limits at +8/+16; GPUSupport converts them to MiB. Bound
        // these by guest RAM and the existing graphics mapping limit.
        const auto bytes = static_cast<std::uint32_t>(
            std::min(state.device_ram_bytes, maximum_memory_bytes));
        word(result, 8U, bytes);
        word(result, 16U, bytes);
        break;
    }
    case 1U:
        if (inband_output_capacity != name_reply_size)
            return MethodResult { iokit_abi::bad_argument, {}, {} };
        result.resize(name_reply_size);
        string(result, 0U, "iLEmu graphics accelerator");
        break;
    default:
        return MethodResult { iokit_abi::unsupported, {}, {} };
    }
    return MethodResult { iokit_abi::success, {}, std::move(result) };
}

} // namespace ilemu::kernel_iokit::graphics
