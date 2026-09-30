// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ilemu {
// A descriptor copy retains the open-file cursor. Assigning a numeric value
// seeks that cursor; copying this object shares it across dup/fork/transfers.
class OpenFileOffset {
    struct State {
        explicit State(std::uint64_t initial) : value(initial) {}
        std::atomic<std::uint64_t> value;
        std::mutex mutex;
    };
public:
    OpenFileOffset() = default;
    explicit OpenFileOffset(std::uint64_t initial)
        : state_(std::make_shared<State>(initial)) {}
    operator std::uint64_t() const
    { return state_ ? state_->value.load(std::memory_order_relaxed) : 0; }
    OpenFileOffset& operator=(std::uint64_t value)
    {
        ensure();
        state_->value.store(value, std::memory_order_relaxed);
        return *this;
    }
    OpenFileOffset& operator+=(std::uint64_t value)
    {
        ensure();
        state_->value.fetch_add(value, std::memory_order_relaxed);
        return *this;
    }
    [[nodiscard]] std::unique_lock<std::mutex> lock()
    {
        ensure();
        return std::unique_lock {state_->mutex};
    }
private:
    void ensure() { if (!state_) state_ = std::make_shared<State>(0); }
    std::shared_ptr<State> state_;
};
} // namespace ilemu
