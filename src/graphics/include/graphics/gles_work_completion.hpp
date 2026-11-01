// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <atomic>
#include <cstdint>

namespace ilemu {
struct GlesWorkResult;

// Retain an exact host command's terminal state without retaining its output
// or queue credit. Host command completion does not imply GPU completion.
class GlesWorkCompletion final {
public:
    enum class Status { Pending, Completed, Failed };

    [[nodiscard]] std::uint64_t sequence() const { return sequence_; }
    [[nodiscard]] bool ready() const
    {
        return status_.load(std::memory_order_acquire) != Status::Pending;
    }
    [[nodiscard]] bool succeeded() const
    {
        return status_.load(std::memory_order_acquire) == Status::Completed;
    }

private:
    friend struct GlesWorkResult;
    // Assigned once under queue admission before this observer is exposed.
    std::uint64_t sequence_ { };
    std::atomic<Status> status_ { Status::Pending };
};
} // namespace ilemu
