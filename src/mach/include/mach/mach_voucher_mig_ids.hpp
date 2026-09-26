// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Darwin Mach voucher MIG identifiers.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.1.97/osfmk/mach/mach_voucher.defs

#pragma once

#include <cstdint>

namespace ilemu::xnu::mig::mach_voucher {

enum class Routine : std::uint32_t {
    extract_attr_content = 5400U,
    extract_attr_recipe = 5401U,
    extract_all_attr_recipes = 5402U,
    attr_command = 5403U,
    debug_info = 5404U,
};

[[nodiscard]] constexpr std::uint32_t id(Routine routine) noexcept
{
    return static_cast<std::uint32_t>(routine);
}

} // namespace ilemu::xnu::mig::mach_voucher
