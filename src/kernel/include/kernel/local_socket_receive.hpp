// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>
#include <utility>

namespace ilemu {
// The caller holds the existing kernel and local socket locks. Keep the
// established coalesced stream contract while committing each successfully
// copied prefix before externalizing the next ancillary record.
class LocalSocketReceive {
public:
    struct Result {
        bool ready {};
        std::uint32_t error {};
        std::size_t transferred {};
    };
    template <typename Records, typename Copy, typename Externalize>
    static Result read(std::deque<std::byte>& source, Records& controls,
        std::uint64_t& position, std::size_t capacity, bool end_of_stream,
        Copy&& copy, Externalize&& externalize)
    {
        while (!controls.empty() && controls.front().byte_offset < position)
            controls.pop_front();
        const bool control_ready = !controls.empty() &&
                                   controls.front().byte_offset == position;
        if (source.empty() && !end_of_stream && !control_ready)
            return {};
        std::vector<std::byte> bytes(std::min(capacity, source.size()));
        std::copy_n(source.begin(), bytes.size(), bytes.begin());
        std::size_t total = 0;
        do {
            if (!controls.empty() && controls.front().byte_offset == position) {
                auto record = std::move(controls.front());
                controls.pop_front();
                if (const auto error = externalize(record.transfers))
                    return {true, error, total};
            }
            auto count = bytes.size() - total;
            if (!controls.empty())
                count = std::min<std::size_t>(count, controls.front().byte_offset - position);
            if (!copy(std::span<const std::byte> {bytes}.subspan(total, count), total))
                return {true, 14U, total}; // EFAULT; failing chunk stays queued
            for (std::size_t i = 0; i < count; ++i)
                source.pop_front();
            position += count;
            total += count;
        } while (total < bytes.size());
        return {true, 0U, total};
    }
};
} // namespace ilemu
