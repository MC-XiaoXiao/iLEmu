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
#include <atomic>
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
    enum class ClientDelivery { Callback, NativeQueues };
    struct Consumer {
        std::uint32_t process;
        std::size_t processor;
        std::uint32_t system;
        bool keyboard_events { };
        bool digitizer_events { true };
        ClientDelivery client_delivery { ClientDelivery::Callback };
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
    struct AmbientLight {
        std::uint32_t lux;
    };
    struct Event {
        std::variant<TouchInput, KeyboardInput, Acceleration, AmbientLight>
            input;
        std::uint64_t timestamp;
        std::uint32_t identity { 1U };
        std::optional<std::array<float, 2>> contact_origin { };
        std::uint64_t input_sequence { };
    };
    struct ObserverDelivery {
        Observer observer;
        Event event;
    };

    void open(Consumer consumer)
    {
        std::lock_guard lock { mutex_ };
        consumer_ = consumer;
        if (consumer.client_delivery == ClientDelivery::NativeQueues)
            observers_.clear();
        events_.clear();
        contact_origin_.reset();
        reset_native_touch_handoff_locked();
        refresh_pending_events_locked();
        receiver_generation_.fetch_add(1, std::memory_order_release);
    }
    void close(std::uint32_t process)
    {
        std::lock_guard lock { mutex_ };
        bool changed = false;
        if (consumer_ && consumer_->process == process) {
            consumer_.reset();
            events_.clear();
            contact_origin_.reset();
            reset_native_touch_handoff_locked();
            changed = true;
        }
        const auto removed = std::erase_if(observers_, [process](const ObserverState& state) {
            return state.observer.process == process;
        });
        if (changed || removed != 0U)
            receiver_generation_.fetch_add(1, std::memory_order_release);
        refresh_pending_events_locked();
    }
    void observe(Observer observer)
    {
        std::lock_guard lock { mutex_ };
        // Native dispatch owns matching, connection state and delivery to
        // client queues. A direct callback would bypass that contract.
        if (consumer_ &&
            consumer_->client_delivery == ClientDelivery::NativeQueues)
            return;
        const auto existing = std::find_if(observers_.begin(), observers_.end(),
            [&](const ObserverState& state) {
                return state.observer.process == observer.process &&
                       state.observer.client == observer.client;
            });
        if (existing == observers_.end()) {
            observers_.push_back(ObserverState { observer, { } });
            receiver_generation_.fetch_add(1, std::memory_order_release);
            return;
        }
        if (existing->observer.processor != observer.processor)
            existing->events.clear();
        existing->observer = observer;
        refresh_pending_events_locked();
        receiver_generation_.fetch_add(1, std::memory_order_release);
    }
    void unobserve(std::uint32_t process, std::uint32_t client)
    {
        std::lock_guard lock { mutex_ };
        const auto removed = std::erase_if(observers_, [&](const ObserverState& state) {
            return state.observer.process == process &&
                   state.observer.client == client;
        });
        if (removed != 0U)
            receiver_generation_.fetch_add(1, std::memory_order_release);
        refresh_pending_events_locked();
    }
    [[nodiscard]] std::uint64_t receiver_generation_snapshot() const noexcept
    {
        return receiver_generation_.load(std::memory_order_acquire);
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
            consumer_ && (!touch || consumer_->digitizer_events) &&
            (!std::holds_alternative<KeyboardInput>(event.input) ||
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
            append(events_, event);
        if (touch) {
            for (auto& state : observers_) {
                if (state.observer.digitizer_events)
                    append(state.events, event);
            }
        }
        events_pending_.store(true, std::memory_order_release);
        delivery_generation_.fetch_add(1, std::memory_order_release);
        return true;
    }
    [[nodiscard]] std::uint64_t delivery_generation_snapshot() const noexcept
    {
        return delivery_generation_.load(std::memory_order_acquire);
    }
    void redrive_pending(std::uint32_t process, std::size_t processor)
    {
        if (has_pending(process, processor))
            delivery_generation_.fetch_add(1, std::memory_order_release);
    }
    [[nodiscard]] std::optional<Event> take(
        std::uint32_t process, std::size_t processor)
    {
        std::lock_guard lock { mutex_ };
        if (!consumer_ || consumer_->process != process ||
            consumer_->processor != processor || events_.empty())
            return std::nullopt;
        prefer_recent_native_move_locked();
        auto event = events_.front();
        events_.pop_front();
        refresh_pending_events_locked();
        return event;
    }
    void note_native_touch_handoff(std::uint32_t process, std::size_t processor,
        const Event& event, bool delivered)
    {
        const auto* touch = std::get_if<TouchInput>(&event.input);
        if (!touch)
            return;
        std::lock_guard lock { mutex_ };
        if (!consumer_ || consumer_->process != process ||
            consumer_->processor != processor ||
            consumer_->client_delivery != ClientDelivery::NativeQueues)
            return;
        if (!delivered || touch->phase != TouchPhase::Move) {
            reset_native_touch_handoff_locked();
            if (delivered && touch->phase == TouchPhase::Down)
                last_native_touch_handoff_ = event;
            return;
        }
        if (!last_native_touch_handoff_ ||
            last_native_touch_handoff_->identity != event.identity) {
            reset_native_touch_handoff_locked();
            return;
        }
        last_native_touch_handoff_ = event;
        if (native_touch_moves_delivered_ < initial_native_move_count)
            ++native_touch_moves_delivered_;
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
        refresh_pending_events_locked();
        return delivery;
    }
    [[nodiscard]] bool has_pending_events() const
    {
        return events_pending_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::optional<std::size_t> pending_processor(
        std::uint32_t process) const
    {
        std::lock_guard lock { mutex_ };
        if (consumer_ && consumer_->process == process && !events_.empty())
            return consumer_->processor;
        const auto observer = std::find_if(observers_.begin(), observers_.end(),
            [&](const ObserverState& state) {
                return state.observer.process == process && !state.events.empty();
            });
        if (observer == observers_.end())
            return std::nullopt;
        return observer->observer.processor;
    }
    [[nodiscard]] bool has_pending(
        std::uint32_t process, std::size_t processor) const
    {
        std::lock_guard lock { mutex_ };
        if (consumer_ && consumer_->process == process &&
            consumer_->processor == processor && !events_.empty())
            return true;
        return std::any_of(observers_.begin(), observers_.end(),
            [&](const ObserverState& state) {
                return state.observer.process == process &&
                       state.observer.processor == processor &&
                       !state.events.empty();
            });
    }

    template<typename Iterator>
    [[nodiscard]] bool has_receiver(std::uint32_t process,
        Iterator first, Iterator last) const
    {
        if (first == last)
            return false;
        auto second = first;
        if (++second == last)
            return is_receiver(process, *first);
        std::lock_guard lock { mutex_ };
        const auto matches = [&](const auto& receiver) {
            return receiver.process == process &&
                std::find(first, last, receiver.processor) != last;
        };
        if (consumer_ && matches(*consumer_))
            return true;
        return std::any_of(observers_.begin(), observers_.end(),
            [&](const ObserverState& state) {
                return matches(state.observer);
            });
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
    // Keep the initial report history until the primary native transport has
    // handed off Down and two following moves. Raw handoff does not establish
    // that a downstream gesture recognizer has transitioned state.
    static constexpr unsigned initial_native_move_count = 2;

    void reset_native_touch_handoff_locked()
    {
        last_native_touch_handoff_.reset();
        native_touch_moves_delivered_ = 0;
    }

    void prefer_recent_native_move_locked()
    {
        if (native_touch_moves_delivered_ < initial_native_move_count)
            return;
        const auto& last = *last_native_touch_handoff_;
        const auto& origin = std::get<TouchInput>(last.input);
        while (events_.size() >= 2) {
            const auto* first = std::get_if<TouchInput>(&events_[0].input);
            const auto* next = std::get_if<TouchInput>(&events_[1].input);
            if (!first || !next || first->phase != TouchPhase::Move ||
                next->phase != TouchPhase::Move ||
                events_[0].identity != last.identity ||
                events_[1].identity != last.identity)
                return;

            // The latest point of the same segment supersedes interior
            // moves. Keep turns, pauses and the final move before a
            // terminal edge or a different event type.
            const auto dx = static_cast<double>(first->x) - origin.x;
            const auto dy = static_cast<double>(first->y) - origin.y;
            const auto step_x = static_cast<double>(next->x) - first->x;
            const auto step_y = static_cast<double>(next->y) - first->y;
            const auto dot = dx * step_x + dy * step_y;
            const auto cross = dx * step_y - dy * step_x;
            if (dot <= 0 || cross * cross > dot * dot / 16.0)
                return;
            events_.pop_front();
        }
    }

    void refresh_pending_events_locked()
    {
        events_pending_.store(!events_.empty() ||
            std::any_of(observers_.begin(), observers_.end(),
                [](const ObserverState& state) { return !state.events.empty(); }),
            std::memory_order_release);
    }
    static void append(std::deque<Event>& queue, const Event& event)
    {
        constexpr std::ptrdiff_t pending_move_limit = 4;
        queue.push_back(event);
        const auto* touch = std::get_if<TouchInput>(&event.input);
        if (!touch || touch->phase != TouchPhase::Move ||
            queue.size() <= static_cast<std::size_t>(pending_move_limit))
            return;

        const auto first = queue.end() - (pending_move_limit + 1);
        if (!std::all_of(first, queue.end() - 1, [&event](const Event& queued) {
                const auto* previous = std::get_if<TouchInput>(&queued.input);
                return previous && previous->phase == TouchPhase::Move &&
                       queued.identity == event.identity;
            }))
            return;

        // Keep the first and last moves and thin the most closely spaced
        // interior samples. A firmware pan needs motion after its first move
        // passes the recognition threshold, even when the host delivers a burst.
        const auto sample_span = [](auto sample) {
            return (sample + 1)->input_sequence - (sample - 1)->input_sequence;
        };
        auto discard = first + 1;
        for (auto sample = discard + 1; sample != queue.end() - 1; ++sample) {
            if (sample_span(sample) < sample_span(discard))
                discard = sample;
        }
        queue.erase(discard);
    }

    struct ObserverState {
        Observer observer;
        std::deque<Event> events;
    };
    mutable std::mutex mutex_;
    std::optional<Consumer> consumer_;
    std::deque<Event> events_;
    std::vector<ObserverState> observers_;
    std::optional<std::array<float, 2>> contact_origin_;
    std::optional<Event> last_native_touch_handoff_;
    unsigned native_touch_moves_delivered_ { };
    // Registration changes invalidate receiver identity. Queued reports and
    // callback completions only need to revisit the registered receivers.
    std::atomic_uint64_t receiver_generation_ { 1 };
    std::atomic_uint64_t delivery_generation_ { 0 };
    std::atomic_bool events_pending_ { false };
};

} // namespace ilemu
