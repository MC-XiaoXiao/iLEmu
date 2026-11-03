// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/file_page_cache.hpp"
#include "foundation/host_resource_controller.hpp"

namespace ilemu {

// Cold file identities are mandatory I/O, independent of optional compilation.
// A separate lane keeps full-file hashing off the warm preparation queue.
// Workers never access guest memory or publish guest mappings.
class HostFileMappingPreparer {
public:
    ~HostFileMappingPreparer();
    // No value means admission failed; a null preparation means invalid input.
    [[nodiscard]] std::optional<std::shared_ptr<FileMappingPreparation>> prepare(
        std::shared_ptr<FilePageCache> cache, const std::filesystem::path& path,
        std::uint64_t file_offset, std::uint32_t size, int source_descriptor = -1);

private:
    // At most 64 retained descriptors/tasks. Mapping preparation retains no
    // speculative page bytes; the pager handles actual guest demand.
    // Cancelled guest continuations release admission when the host work ends.
    struct Admission;
    std::shared_ptr<std::atomic<std::size_t>> admitted_ {
        std::make_shared<std::atomic<std::size_t>>(0)
    };
    HostResourceController worker_;
    HostResourceController identity_worker_;
};

} // namespace ilemu
