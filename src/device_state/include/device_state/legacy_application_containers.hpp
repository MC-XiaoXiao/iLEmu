/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <filesystem>

#include "foundation/darwin_version.hpp"

namespace ilemu {

void prepare_legacy_application_containers(
    const std::filesystem::path& rootfs, DarwinVersion version);

} // namespace ilemu
