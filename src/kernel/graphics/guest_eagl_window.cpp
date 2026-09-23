// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "guest_eagl_window.hpp"

#include <limits>

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/surface_store.hpp"

namespace ilemu {
namespace {
    constexpr std::uint32_t minimum_surface_window_version = 2U;
    constexpr std::uint32_t configure_offset = 0x04U;
    constexpr std::uint32_t acquire_offset = 0x0cU;
    constexpr std::uint32_t present_offset = 0x10U;

    std::optional<std::uint32_t> callback(
        UserlandHleCall& call, std::uint32_t window, std::uint32_t offset)
    {
        if (window == 0U ||
            window > std::numeric_limits<std::uint32_t>::max() - offset)
            return std::nullopt;
        const auto value = call.memory().read32(window + offset);
        return value && *value != 0U ? value : std::nullopt;
    }
}

std::optional<GuestEaglWindow> GuestEaglWindow::open(
    UserlandHleCall& call, std::uint32_t address)
{
    if (address == 0U)
        return std::nullopt;
    const auto version = call.memory().read32(address);
    if (!version ||
        *version < minimum_surface_window_version ||
        !callback(call, address, configure_offset) ||
        !callback(call, address, acquire_offset) ||
        !callback(call, address, present_offset)) {
        return std::nullopt;
    }
    return GuestEaglWindow { address };
}

void GuestEaglWindow::acquire(
    UserlandHleCall& call, StorageCompletion completion) const
{
    const auto function = callback(call, address_, acquire_offset);
    if (!function) {
        completion(call, 0U);
        return;
    }
    call.cpu().registers()[0] = address_;
    if (!call.call_guest_callback(*function,
            [completion](UserlandHleCall& returned) {
                completion(returned, returned.cpu().registers()[0]);
            })) {
        completion(call, 0U);
    }
}

void GuestEaglWindow::configure(
    UserlandHleCall& call, StorageCompletion completion) const
{
    const auto function = callback(call, address_, configure_offset);
    if (!function) {
        completion(call, 0U);
        return;
    }
    auto& registers = call.cpu().registers();
    registers[0] = address_;
    registers[1] = surface_pixel_format_bgra;
    registers[2] = 0U;
    if (!call.call_guest_callback(*function,
            [window = *this,
                completion](UserlandHleCall& returned) {
                if (returned.cpu().registers()[0] != 1U) {
                    completion(returned, 0U);
                    return;
                }
                window.acquire(returned, completion);
            })) {
        completion(call, 0U);
    }
}

void GuestEaglWindow::present(
    UserlandHleCall& call, PresentCompletion completion) const
{
    const auto function = callback(call, address_, present_offset);
    if (!function) {
        completion(call, false, 0U);
        return;
    }
    auto& registers = call.cpu().registers();
    registers[0] = address_;
    registers[1] = 1U;
    if (!call.call_guest_callback(*function,
            [window = *this,
                completion](UserlandHleCall& returned) {
                if (returned.cpu().registers()[0] != 1U) {
                    completion(returned, false, 0U);
                    return;
                }
                window.acquire(returned,
                    [completion](UserlandHleCall& acquired,
                        std::uint32_t surface) {
                        completion(acquired, true, surface);
                    });
            })) {
        completion(call, false, 0U);
    }
}

} // namespace ilemu
