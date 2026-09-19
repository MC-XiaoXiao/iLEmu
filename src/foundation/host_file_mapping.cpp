// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/host_file_mapping.hpp"

#include <utility>

namespace ilemu {

HostFileMappingPreparer::~HostFileMappingPreparer()
{
    worker_.wait_idle();
}

std::shared_ptr<FileMappingPreparation> HostFileMappingPreparer::prepare(
    std::shared_ptr<FilePageCache> cache, const std::filesystem::path& path,
    std::uint64_t file_offset, std::uint32_t size)
{
    auto preparation = FileMappingPreparation::begin(
        std::move(cache), path, file_offset, size);
    if (preparation && !preparation->ready()) {
        const auto task = worker_.submit(HostWorkKind::Maintenance, std::nullopt,
            [preparation] { preparation->complete(); });
        if (!task)
            return { };
    }
    return preparation;
}

} // namespace ilemu
