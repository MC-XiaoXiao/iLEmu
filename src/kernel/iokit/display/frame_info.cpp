// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Enumerate the framebuffer's frame-information channels through its native
// IOConnectCallMethod contract. The guest framework owns the resulting table.

#include "kernel/kernel_iokit_display.hpp"

#include "kernel/iokit_abi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace ilemu::kernel_iokit::display {

std::optional<MethodResult> dispatch_frame_info_method(
    std::uint32_t selector, std::span<const std::uint64_t> scalar_input,
    std::span<const std::byte> inband_input,
    std::uint32_t scalar_output_capacity,
    std::uint32_t inband_output_capacity)
{
    if (selector != static_cast<std::uint32_t>(
                        iokit_abi::MobileFramebufferSelector::
                            CopySupportedFrameInfoName))
        return std::nullopt;

    // The framework asks for one index at a time, with the first response's
    // scalar indicating the full table size. Names are NUL-terminated strings
    // in the method's inline output structure.
    constexpr std::array<std::string_view, 2> names {
        "ColorElements", "TimingElements"
    };
    if (scalar_input.size() != 1U || !inband_input.empty() ||
        scalar_output_capacity < 1U || scalar_input[0] >= names.size())
        return MethodResult { iokit_abi::bad_argument, { }, { } };

    const auto name = names[static_cast<std::size_t>(scalar_input[0])];
    if (inband_output_capacity < name.size() + 1U)
        return MethodResult { iokit_abi::bad_argument, { }, { } };

    std::vector<std::byte> output;
    output.reserve(name.size() + 1U);
    for (const auto character : name)
        output.push_back(static_cast<std::byte>(character));
    output.push_back(std::byte { 0 });
    return MethodResult { iokit_abi::success, { names.size() },
        std::move(output) };
}

} // namespace ilemu::kernel_iokit::display
