// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <array>
#include <cassert>
#include <cstdint>

namespace ilemu::xnu::ipc {
// ARM32 ipc_table_fill: 63 sizes, eight-byte request entries, 4 KiB pages.
// Slot zero is the table header. Actual requests remain in the kernel's
// task/name index; this tracks native capacity without duplicating storage.
class PortRequestTable {
public:
    enum class Result : std::uint32_t { Success = 0, NoSpace = 3, ResourceShortage = 6 };

    [[nodiscard]] std::uint32_t capacity() const { return sizes_[size_index_]; }
    Result grow(std::uint32_t target)
    {
        if (target != 0U && target <= capacity())
            return Result::Success;
        auto next = size_index_ + 1U;
        while (next < sizes_.size() && target > sizes_[next])
            ++next;
        if (next == sizes_.size())
            return target == 0U ? Result::ResourceShortage : Result::NoSpace;
        // ipc_port_{dn,request}_grow publishes only if the old table is null
        // or its immediate successor is the selected size (XNU792--4903).
        // A nonempty table's explicit multi-step target succeeds unchanged.
        if (size_index_ == 0U || next == size_index_ + 1U)
            size_index_ = next;
        return Result::Success;
    }
    Result allocate()
    {
        if (used_ + 1U >= capacity()) {
            const auto result = grow(0U);
            if (result != Result::Success)
                return result;
        }
        ++used_;
        return Result::Success;
    }
    void release()
    {
        assert(used_ != 0U);
        --used_;
    }

private:
    static constexpr auto sizes_ = [] {
        std::array<std::uint32_t, 64> sizes { };
        std::uint32_t index = 1U, bytes = 16U;
        for (; bytes < 4096U; bytes <<= 1U)
            sizes[index++] = bytes / 8U;
        for (std::uint32_t increment = 4096U; index < sizes.size();) {
            for (std::uint32_t period = 0U; period < 15U && index < sizes.size();
                 ++period, bytes += increment)
                sizes[index++] = bytes / 8U;
            if (increment < 32768U)
                increment <<= 1U;
        }
        return sizes;
    }();
    std::uint32_t size_index_ { };
    std::uint32_t used_ { };
};
} // namespace ilemu::xnu::ipc
