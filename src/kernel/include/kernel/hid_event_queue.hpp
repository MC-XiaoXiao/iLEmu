// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Represent shared guest HID event queues and their notification
// state.

#pragma once

#include "foundation/touch_input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <variant>

namespace ilemu {

class HidEventQueue {
public:
    struct Consumer {
        std::uint32_t process;
        std::size_t processor;
        std::uint32_t system;
        bool keyboard_events { };
    };
    struct KeyboardInput {
        std::uint32_t usage_page;
        std::uint32_t usage;
        bool down;
    };
    struct Acceleration {
        float x;
        float y;
        float z;
    };
    struct Event {
        std::variant<TouchInput, KeyboardInput, Acceleration> input;
        std::uint64_t timestamp;
        std::uint32_t identity { 1U };
        std::optional<std::array<float, 2>> contact_origin { };
    };

    void open(Consumer consumer)
    {
        std::lock_guard lock { mutex_ };
        consumer_ = consumer;
        events_.clear();
        contact_origin_.reset();
    }
    void close(std::uint32_t process)
    {
        std::lock_guard lock { mutex_ };
        if (consumer_ && consumer_->process == process) {
            consumer_.reset();
            events_.clear();
            contact_origin_.reset();
        }
    }
    [[nodiscard]] std::optional<Consumer> consumer() const
    {
        std::lock_guard lock { mutex_ };
        return consumer_;
    }
    [[nodiscard]] bool enqueue(Event event)
    {
        std::lock_guard lock { mutex_ };
        if (!consumer_ || (std::holds_alternative<KeyboardInput>(event.input) &&
                              !consumer_->keyboard_events))
            return false;
        if (const auto* touch = std::get_if<TouchInput>(&event.input)) {
            if (touch->phase == TouchPhase::Down)
                contact_origin_ = std::array { touch->x, touch->y };
            event.contact_origin = contact_origin_;
            if (touch->phase == TouchPhase::Up ||
                touch->phase == TouchPhase::Cancel)
                contact_origin_.reset();
        }
        events_.push_back(event);
        return true;
    }
    [[nodiscard]] std::optional<Event> take(
        std::uint32_t process, std::size_t processor)
    {
        std::lock_guard lock { mutex_ };
        if (!consumer_ || consumer_->process != process ||
            consumer_->processor != processor || events_.empty())
            return std::nullopt;
        auto event = events_.front();
        events_.pop_front();
        return event;
    }

private:
    mutable std::mutex mutex_;
    std::optional<Consumer> consumer_;
    std::deque<Event> events_;
    std::optional<std::array<float, 2>> contact_origin_;
};

} // namespace ilemu
