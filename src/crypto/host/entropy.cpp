// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "crypto/host_entropy.hpp"

#include <limits>
#include <openssl/rand.h>

namespace ilemu {

bool fill_host_entropy(std::span<std::byte> bytes)
{
    if (bytes.empty())
        return true;
    if (bytes.size() > static_cast<std::size_t>(
                           std::numeric_limits<int>::max()))
        return false;
    return RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()),
               static_cast<int>(bytes.size())) == 1;
}

} // namespace ilemu
