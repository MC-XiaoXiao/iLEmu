// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Represent shared guest HID event queues and their notification
// state.

#pragma once

#include "foundation/touch_input.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace ilemu {

class HidEventQueue {
public:
    struct Consumer {
        std::uint32_t process;
        std::size_t processor;
        std::uint32_t system;
        bool keyboard_events { };
    };
    struct Observer {
        std::uint32_t process;
        std::size_t processor;
        std::uint32_t client;
        std::uint32_t callback;
        std::uint32_t target;
        std::uint32_t refcon;
        bool digitizer_events { };
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
    struct ObserverDelivery {
        Observer observer;
        Event event;
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
        std::erase_if(observers_, [process](const ObserverState& state) {
            return state.observer.process == process;
        });
    }
    void observe(Observer observer)
    {
        std::lock_guard lock { mutex_ };
        const auto existing = std::find_if(observers_.begin(), observers_.end(),
            [&](const ObserverState& state) {
                return state.observer.process == observer.process &&
                       state.observer.client == observer.client;
            });
        if (existing == observers_.end()) {
            observers_.push_back(ObserverState { observer, { } });
            return;
        }
        if (existing->observer.processor != observer.processor)
            existing->events.clear();
        existing->observer = observer;
    }
    void unobserve(std::uint32_t process, std::uint32_t client)
    {
        std::lock_guard lock { mutex_ };
        std::erase_if(observers_, [&](const ObserverState& state) {
            return state.observer.process == process &&
                   state.observer.client == client;
        });
    }
    [[nodiscard]] std::optional<Consumer> consumer() const
    {
        std::lock_guard lock { mutex_ };
        return consumer_;
    }
    [[nodiscard]] bool enqueue(Event event)
    {
        std::lock_guard lock { mutex_ };
        const auto touch = std::holds_alternative<TouchInput>(event.input);
        const auto primary =
            consumer_ && (!std::holds_alternative<KeyboardInput>(event.input) ||
                             consumer_->keyboard_events);
        const auto observed = std::any_of(observers_.begin(), observers_.end(),
            [touch](const ObserverState& state) {
                return touch && state.observer.digitizer_events;
            });
        if (!primary && !observed)
            return false;
        if (const auto* contact = std::get_if<TouchInput>(&event.input)) {
            if (contact->phase == TouchPhase::Down)
                contact_origin_ = std::array { contact->x, contact->y };
            event.contact_origin = contact_origin_;
            if (contact->phase == TouchPhase::Up ||
                contact->phase == TouchPhase::Cancel)
                contact_origin_.reset();
        }
        if (primary)
            events_.push_back(event);
        if (touch) {
            for (auto& state : observers_) {
                if (state.observer.digitizer_events)
                    state.events.push_back(event);
            }
        }
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
    [[nodiscard]] std::optional<ObserverDelivery> take_observer(
        std::uint32_t process, std::size_t processor)
    {
        std::lock_guard lock { mutex_ };
        const auto observer = std::find_if(observers_.begin(), observers_.end(),
            [&](const ObserverState& state) {
                return state.observer.process == process &&
                       state.observer.processor == processor &&
                       !state.events.empty();
            });
        if (observer == observers_.end())
            return std::nullopt;
        ObserverDelivery delivery { observer->observer,
            observer->events.front() };
        observer->events.pop_front();
        return delivery;
    }
    [[nodiscard]] bool is_receiver(
        std::uint32_t process, std::size_t processor) const
    {
        std::lock_guard lock { mutex_ };
        if (consumer_ && consumer_->process == process &&
            consumer_->processor == processor) {
            return true;
        }
        return std::any_of(observers_.begin(), observers_.end(),
            [&](const ObserverState& state) {
                return state.observer.process == process &&
                       state.observer.processor == processor;
            });
    }

private:
    struct ObserverState {
        Observer observer;
        std::deque<Event> events;
    };
    mutable std::mutex mutex_;
    std::optional<Consumer> consumer_;
    std::deque<Event> events_;
    std::vector<ObserverState> observers_;
    std::optional<std::array<float, 2>> contact_origin_;
};

} // namespace ilemu
