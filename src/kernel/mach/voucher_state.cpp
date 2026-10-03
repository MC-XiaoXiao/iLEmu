// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// XNU voucher recipe execution and ATM activity identity:
// https://github.com/apple-oss-distributions/xnu/blob/xnu-3248.20.55/osfmk/ipc/ipc_voucher.c
// https://github.com/apple-oss-distributions/xnu/blob/xnu-3248.20.55/osfmk/atm/atm.c
#include "kernel/mach_voucher_state.hpp"
#include "kernel/darwin_abi.hpp"

#include <algorithm>

namespace ilemu {
namespace {
    constexpr std::uint32_t all_keys = 0xffff'ffffU;
    constexpr std::uint32_t atm_key = 1U;
    constexpr std::uint32_t copy = 1U;
    constexpr std::uint32_t remove = 2U;
    constexpr std::uint32_t set_handle = 3U;
    constexpr std::uint32_t redeem = 10U;
    constexpr std::uint32_t atm_null = 501U;
    constexpr std::uint32_t atm_create = 510U;
    constexpr std::uint32_t atm_register = 511U;
    constexpr std::uint32_t atm_find_min_subaid = 4U;
    constexpr std::uint32_t atm_unregister_action = 5U;
    constexpr std::uint32_t atm_register_action = 6U;
    constexpr std::uint32_t atm_get_subaid = 7U;
    constexpr std::size_t header_size = 16U;

    std::uint64_t read_integer(std::span<const std::byte> bytes)
    {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i)
            value |= std::to_integer<std::uint64_t>(bytes[i]) << (i * 8U);
        return value;
    }

    void append_integer(std::vector<std::byte>& bytes,
        std::uint64_t value, std::size_t size)
    {
        for (std::size_t i = 0; i < size; ++i)
            bytes.push_back(static_cast<std::byte>(value >> (i * 8U)));
    }
}

std::uint32_t MachVoucherState::create(std::span<const std::byte> recipes,
    const Resolver& resolve,
    const std::map<std::uint32_t, MachVoucherState>& vouchers,
    std::uint64_t& next_activity_id, std::uint32_t pid)
{
    std::vector<std::pair<std::shared_ptr<Activity>, std::uint64_t>> registrations;
    const auto activity_exists = [&](std::uint64_t id) {
        const auto contains = [id](const MachVoucherState& voucher) {
            const auto found = voucher.attributes_.find(atm_key);
            return found != voucher.attributes_.end() &&
                   read_integer(found->second.content) == id;
        };
        return contains(*this) || std::ranges::any_of(vouchers,
            [&](const auto& entry) { return contains(entry.second); });
    };
    while (!recipes.empty()) {
        if (recipes.size() < header_size)
            return darwin::mach::invalid_argument;
        const auto key = static_cast<std::uint32_t>(read_integer(recipes.first(4)));
        const auto command = read_integer(recipes.subspan(4, 4));
        const auto previous = static_cast<std::uint32_t>(read_integer(recipes.subspan(8, 4)));
        const auto size = read_integer(recipes.subspan(12, 4));
        if (size > recipes.size() - header_size)
            return darwin::mach::invalid_argument;
        const auto content = recipes.subspan(header_size, size);
        recipes = recipes.subspan(header_size + size);
        const auto* source = previous ? resolve(previous) : nullptr;
        if (previous && !source)
            return darwin::mach::invalid_argument;
        if (command == set_handle)
            return darwin::mach::invalid_capability;
        if (command == copy || command == remove) {
            if (!content.empty())
                return darwin::mach::invalid_argument;
            if (command == copy) {
                if (!source) continue;
                if (key == all_keys) attributes_ = source->attributes_;
                else if (const auto found = source->attributes_.find(key);
                         found != source->attributes_.end())
                    attributes_[key] = found->second;
                else attributes_.erase(key);
            } else {
                std::erase_if(attributes_, [&](const auto& entry) {
                    if (key != all_keys && entry.first != key) return false;
                    if (!source) return true;
                    const auto found = source->attributes_.find(entry.first);
                    return found != source->attributes_.end() && found->second == entry.second;
                });
            }
            continue;
        }
        if (key == atm_key) {
            if (command == atm_create) {
                if (!content.empty() && content.size() != 8U)
                    return darwin::mach::invalid_argument;
                auto id = read_integer(content);
                if (content.empty()) {
                    do { id = next_activity_id++; } while (!id || activity_exists(id));
                } else if (activity_exists(id)) {
                    return darwin::mach::invalid_argument;
                }
                Attribute attribute { atm_null, {}, std::make_shared<Activity>() };
                append_integer(attribute.content, id, 8U);
                attributes_[key] = std::move(attribute);
            } else if (command == atm_register) {
                // A null previous voucher uses the value accumulated so far.
                if (!source) source = this;
                const auto found = source->attributes_.find(key);
                if (found == source->attributes_.end()) {
                    attributes_.erase(key);
                    continue;
                }
                if (content.size() != 8U)
                    return darwin::mach::invalid_argument;
                attributes_[key] = found->second;
                registrations.emplace_back(found->second.activity, read_integer(content));
            } else {
                return darwin::mach::invalid_argument;
            }
        } else if (command == redeem) {
            // User-data values are immutable and redeem to the same value.
            if (!source) source = this;
            if (key == all_keys) attributes_ = source->attributes_;
            else if (const auto found = source->attributes_.find(key);
                     found != source->attributes_.end())
                attributes_[key] = found->second;
            else attributes_.erase(key);
        } else {
            // Preserve opaque resource-manager attributes used by the other
            // host services; their content is distinct from the recipe header.
            attributes_[key] = Attribute { static_cast<std::uint32_t>(command),
                { content.begin(), content.end() }, {} };
        }
    }
    for (const auto& [activity, guard] : registrations)
        activity->register_task(pid, guard);
    return darwin::mach::success;
}

std::vector<std::byte> MachVoucherState::extract(
    std::uint32_t key, bool include_recipe) const
{
    const auto found = attributes_.find(key);
    if (found == attributes_.end()) return {};
    const auto& attribute = found->second;
    if (!include_recipe) return attribute.content;
    std::vector<std::byte> result;
    append_integer(result, key, 4);
    append_integer(result, attribute.command, 4);
    append_integer(result, 0, 4);
    append_integer(result, attribute.content.size(), 4);
    result.insert(result.end(), attribute.content.begin(), attribute.content.end());
    return result;
}

bool MachVoucherState::operator==(const MachVoucherState& other) const
{
    return attributes_ == other.attributes_;
}

std::uint32_t MachVoucherState::command(std::uint32_t key,
    std::uint32_t command, std::span<const std::byte> input,
    std::uint32_t capacity, std::uint32_t pid,
    std::uint64_t& next_subactivity_id, std::vector<std::byte>& output)
{
    if (key != atm_key) return darwin::mach::not_supported;
    if (command == atm_find_min_subaid) {
        // XNU reports the maximum 32-bit sub-ID for each supplied activity.
        const auto count = input.size() / 8U;
        if (count > capacity / 8U || count > 256U)
            return darwin::mach::failure;
        for (std::size_t i = 0; i < count; ++i)
            append_integer(output, 0xffff'ffffU, 8U);
        return darwin::mach::success;
    }
    if (command == atm_get_subaid) {
        if (capacity != 8U) return darwin::mach::failure;
        append_integer(output, next_subactivity_id++, 8U);
        return darwin::mach::success;
    }
    if (command != atm_unregister_action && command != atm_register_action)
        return darwin::mach::not_supported;
    const auto found = attributes_.find(key);
    if (found == attributes_.end() || !found->second.activity)
        return darwin::mach::failure;
    if (input.size() != 8U) return darwin::mach::invalid_argument;
    auto& activity = *found->second.activity;
    const auto guard = read_integer(input);
    if (command == atm_register_action) {
        activity.register_task(pid, guard);
        return darwin::mach::success;
    }
    const auto registration = activity.registrations.find(pid);
    if (registration == activity.registrations.end())
        return darwin::mach::failure;
    const auto result = registration->second.guard == guard
        ? darwin::mach::success : darwin::mach::invalid_value;
    if (result == darwin::mach::success) registration->second.guard = 0;
    if (--registration->second.references == 0)
        activity.registrations.erase(registration);
    return result;
}

} // namespace ilemu
