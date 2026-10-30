// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "network/darwin_network_abi.hpp"

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ilemu {
class AddressSpace;
class Cpu;
class Output;

// Read the firmware's own CF conversion sites. Layouts belong to a consumer
// and a wire shape, not to an OS release or to a host display backend.
class Apple80211LayoutResolver {
public:
    void reset();
    [[nodiscard]] bool write_signal(AddressSpace&, Cpu&, Output&,
        std::uint32_t address, std::uint32_t size, std::int32_t signal);
    [[nodiscard]] std::optional<
        darwin::network::apple80211_driver::NetworkRecordLayout>
    network_record(AddressSpace&, Cpu&, Output&, std::uint32_t address,
        std::uint32_t size);

private:
    struct SignalLayout {
        std::uint32_t count;
        std::uint32_t unit;
        std::vector<std::uint32_t> values;
    };
    std::map<std::pair<std::uint32_t, std::uint32_t>, SignalLayout> signals_;
    std::mutex mutex_;
    std::map<std::pair<std::uint32_t, std::uint32_t>,
        darwin::network::apple80211_driver::NetworkRecordLayout>
        networks_;
};
} // namespace ilemu
