// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "foundation/host_resource_controller.hpp"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>

namespace ilemu {
struct HostFileRenameResult {
    std::error_code error;
    bool renamed { };
    bool directory { };
    std::optional<std::uint32_t> replaced_identity;
};
class HostFileRenameRequest {
public:
    [[nodiscard]] std::optional<HostFileRenameResult> result() const;

private:
    friend class HostFileRenamer;
    HostFileRenameResult result_;
    std::atomic<bool> finished_ { false };
};
// A single bounded worker preserves submission order. It owns host paths only;
// guest state and completion effects remain on the emulation thread.
class HostFileRenamer {
public:
    ~HostFileRenamer();
    [[nodiscard]] std::shared_ptr<HostFileRenameRequest> rename(
        std::filesystem::path root, std::filesystem::path source,
        std::filesystem::path destination);

private:
    HostResourceController worker_;
};
} // namespace ilemu
