// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

namespace ilemu {
// Copies retain the open description; numeric assignments update its status.
class OpenFileStatusFlags {
    struct State {
        explicit State(std::uint32_t initial) : value(initial) {}
        std::atomic<std::uint32_t> value;
    };
public:
    OpenFileStatusFlags() = default;
    explicit OpenFileStatusFlags(std::uint32_t initial)
        : state_(std::make_shared<State>(initial)) {}
    operator std::uint32_t() const
    { return state_ ? state_->value.load(std::memory_order_relaxed) : 0; }
    OpenFileStatusFlags& operator=(std::uint32_t value)
    {
        ensure();
        state_->value.store(value, std::memory_order_relaxed);
        return *this;
    }
    void replace(std::uint32_t mask, std::uint32_t value)
    {
        ensure();
        auto previous = state_->value.load(std::memory_order_relaxed);
        while (!state_->value.compare_exchange_weak(previous,
            (previous & ~mask) | (value & mask), std::memory_order_relaxed)) {
        }
    }
private:
    void ensure() { if (!state_) state_ = std::make_shared<State>(0); }
    std::shared_ptr<State> state_;
};
} // namespace ilemu
