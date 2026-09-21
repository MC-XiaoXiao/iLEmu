// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Adapt guest IOHIDEventSystem calls to emulator input services.

#pragma once

#include "kernel/hid_accelerometer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace ilemu {

class Cpu;
class UserlandHleCall;
class UserlandHleRegistry;
struct KernelSharedState;

class HidEventSystemHle {
public:
    explicit HidEventSystemHle(UserlandHleRegistry& registry);
    void set_shared_state(std::shared_ptr<KernelSharedState> state);
    void reset(std::uint32_t process);
    [[nodiscard]] bool is_event_consumer(
        std::uint32_t process, std::size_t processor) const;
    [[nodiscard]] std::optional<std::uint64_t> next_sample_deadline() const
    {
        return delivering_processors_.contains(consumer_processor_)
                   ? std::nullopt
                   : accelerometer_.next_deadline();
    }
    [[nodiscard]] bool prepare_pending_event(
        Cpu& cpu, std::uint32_t process, std::uint32_t svc_immediate);

private:
    struct EventSystemClient {
        std::uint32_t process { };
        std::uint32_t client { };
        std::optional<std::size_t> processor;
        std::uint32_t callback { };
        std::uint32_t target { };
        std::uint32_t refcon { };
        bool digitizer_matching { };
    };

    void refresh_event_client(std::uint32_t client);

    UserlandHleRegistry& registry_;
    std::shared_ptr<KernelSharedState> state_;
    std::uint32_t consumer_process_ { };
    std::size_t consumer_processor_ { };
    std::unordered_map<std::uint32_t, EventSystemClient> event_clients_;
    std::unordered_set<std::size_t> delivering_processors_;
    HidAccelerometer accelerometer_;
};

} // namespace ilemu
