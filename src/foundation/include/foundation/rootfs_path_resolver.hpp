// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Resolve guest paths and symbolic links within the selected firmware
// root filesystem.

#pragma once

#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace ilemu {

class RootfsPathResolver {
public:
    explicit RootfsPathResolver(std::filesystem::path rootfs)
        : rootfs_ { std::move(rootfs) }
    {
    }

    // Optionally append inspected host paths, retaining symlink dependencies
    // that are absent from the final resolved path.
    [[nodiscard]] std::filesystem::path resolve(std::string_view guest_path,
        const std::filesystem::path& guest_working_directory = "/",
        bool follow_final_symlink = true,
        std::vector<std::filesystem::path>* traversed_paths = nullptr) const;

private:
    std::filesystem::path rootfs_;
};

} // namespace ilemu
