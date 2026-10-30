// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ilemu {
class UserlandHleRegistry;

// Observe the public C call contract while the firmware builds its own MIG
// message. The original function constructs the request and handles its reply.
class IOKitMethodHle {
public:
    struct Call {
        std::uint32_t connection;
        std::uint32_t selector;
        std::uint32_t scalar_capacity;
        std::uint32_t structure_capacity;
        std::optional<bool> scalar_first;
    };

    explicit IOKitMethodHle(UserlandHleRegistry& registry);
    void reset();
    [[nodiscard]] Call* current(std::size_t processor);

private:
    std::unordered_map<std::size_t, std::vector<Call>> calls_;
    std::optional<bool> scalar_first_;
};
} // namespace ilemu
