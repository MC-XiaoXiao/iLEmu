// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/firmware_routine.hpp"

#include "foundation/address_space.hpp"
#include "foundation/content_identity.hpp"

#include <algorithm>
#include <span>

namespace ilemu {

bool FirmwareRoutine::matches(AddressSpace& memory, std::uint32_t entry) const
{
    const auto bytes = memory.read_bytes(entry, size);
    return bytes && std::ranges::equal(
        sha256(*bytes).digest, std::as_bytes(std::span { digest }));
}

} // namespace ilemu
