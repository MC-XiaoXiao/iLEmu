// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace ilemu {

// Immutable voucher attributes share the lifetime of their resource values.
// The caller holds mach_mutex, including while resolving previous vouchers.
class MachVoucherState {
public:
    using Resolver = std::function<const MachVoucherState*(std::uint32_t)>;
    std::uint32_t create(std::span<const std::byte> recipes,
        const Resolver& resolve,
        const std::map<std::uint32_t, MachVoucherState>& vouchers,
        std::uint64_t& next_activity_id, std::uint32_t pid);
    [[nodiscard]] std::vector<std::byte> extract(
        std::uint32_t key, bool include_recipe) const;
    std::uint32_t command(std::uint32_t key, std::uint32_t command,
        std::span<const std::byte> input, std::uint32_t capacity,
        std::uint32_t pid, std::uint64_t& next_subactivity_id,
        std::vector<std::byte>& output);
    [[nodiscard]] bool empty() const { return attributes_.empty(); }
    [[nodiscard]] bool operator==(const MachVoucherState& other) const;

private:
    struct Activity {
        struct Registration {
            std::uint64_t guard {};
            std::uint32_t references {};
        };
        std::map<std::uint32_t, Registration> registrations;
        void register_task(std::uint32_t pid, std::uint64_t guard)
        {
            auto& registration = registrations[pid];
            registration.guard = guard;
            ++registration.references;
        }
    };
    struct Attribute {
        std::uint32_t command {};
        std::vector<std::byte> content;
        std::shared_ptr<Activity> activity;
        bool operator==(const Attribute& other) const
        {
            return command == other.command && content == other.content;
        }
    };
    std::map<std::uint32_t, Attribute> attributes_;
};

} // namespace ilemu
