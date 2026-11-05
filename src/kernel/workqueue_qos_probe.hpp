// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "kernel/darwin_kqueue_abi.hpp"
#include <algorithm>
#include <boost/container/small_vector.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ilemu {

// Probe eligible buckets together while retaining their registration order.
// This state lasts for one workqueue request; delivery still uses one bucket.
class WorkqueueQosProbe {
public:
    void add_bucket(std::uint32_t bucket) { buckets_.push_back(bucket); }
    [[nodiscard]] bool empty() const { return buckets_.empty(); }

    [[nodiscard]] std::optional<std::size_t> candidate_rank(std::uint32_t qos) const
    {
        const auto bucket = darwin::kqueue::workqueue_qos_bucket(qos);
        const auto found = std::find(buckets_.begin(), buckets_.end(), bucket);
        if (found == buckets_.end())
            return std::nullopt;
        const auto rank = static_cast<std::size_t>(found - buckets_.begin());
        if (ready_rank_ && rank >= *ready_rank_)
            return std::nullopt;
        return rank;
    }

    // The first eligible bucket is final; other ready buckets remain provisional.
    bool record_ready(std::size_t rank)
    {
        ready_rank_ = rank;
        return rank == 0U;
    }

    [[nodiscard]] std::optional<std::uint32_t> selected_bucket() const
    {
        return ready_rank_ ? std::optional { buckets_[*ready_rank_] } : std::nullopt;
    }

private:
    boost::container::small_vector<std::uint32_t, 8> buckets_;
    std::optional<std::size_t> ready_rank_;
};

} // namespace ilemu
