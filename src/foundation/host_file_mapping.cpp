// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/host_file_mapping.hpp"

#include <utility>

namespace ilemu {
namespace {
    constexpr std::size_t maximum_pending_mappings = 64;
    constexpr std::size_t maximum_prepared_mapping_bytes = 512U * 1024U;
}

struct HostFileMappingPreparer::Admission {
    explicit Admission(std::shared_ptr<std::atomic<std::size_t>> count)
        : count_ { std::move(count) }
    {
        count_->fetch_add(1, std::memory_order_relaxed);
    }
    ~Admission() { count_->fetch_sub(1, std::memory_order_release); }
    std::shared_ptr<std::atomic<std::size_t>> count_;
};

HostFileMappingPreparer::~HostFileMappingPreparer()
{
    worker_.wait_idle();
}

std::optional<std::shared_ptr<FileMappingPreparation>>
HostFileMappingPreparer::prepare(
    std::shared_ptr<FilePageCache> cache, const std::filesystem::path& path,
    std::uint64_t file_offset, std::uint32_t size, int source_descriptor)
{
    // The serial kernel is the only producer; the worker only releases slots.
    if (admitted_->load(std::memory_order_acquire) >= maximum_pending_mappings)
        return std::nullopt;
    auto admission = std::make_shared<Admission>(admitted_);
    auto preparation = FileMappingPreparation::begin(
        std::move(cache), path, file_offset, size, source_descriptor, true);
    if (preparation) {
        preparation->retain_admission(admission);
        const auto task = worker_.submit(HostWorkKind::Maintenance, std::nullopt,
            [pending = std::weak_ptr { preparation },
                admission = std::move(admission)] {
                if (auto preparation = pending.lock())
                    preparation->complete(maximum_prepared_mapping_bytes);
            });
        if (!task)
            return std::nullopt;
    }
    return preparation;
}

} // namespace ilemu
