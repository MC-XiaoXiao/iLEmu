// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/file_page_cache.hpp"
#include "foundation/host_resource_controller.hpp"

namespace ilemu {

// Cold file identities are mandatory I/O, independent of optional compilation.
// The worker never accesses guest memory or publishes guest mappings.
class HostFileMappingPreparer {
public:
    ~HostFileMappingPreparer();
    [[nodiscard]] std::shared_ptr<FileMappingPreparation> prepare(
        std::shared_ptr<FilePageCache> cache, const std::filesystem::path& path,
        std::uint64_t file_offset, std::uint32_t size);

private:
    HostResourceController worker_;
};

} // namespace ilemu
