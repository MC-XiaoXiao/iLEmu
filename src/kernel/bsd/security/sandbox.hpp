// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Declare guest sandbox query results and dispatch interfaces.

#pragma once

#include <cstdint>

namespace ilemu {

class AddressSpace;

namespace bsd::sandbox {

enum class CallResult {
    Unsupported,
    Success,
    BadAddress,
};

[[nodiscard]] CallResult dispatch(
    AddressSpace& memory, std::uint32_t operation, std::uint32_t argument);

} // namespace bsd::sandbox
} // namespace ilemu
