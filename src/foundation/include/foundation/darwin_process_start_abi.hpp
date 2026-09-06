// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Define the firmware capability profile for the initial Darwin
// apple-vector entries.

#pragma once

#include <cstdint>

namespace ilemu {

// Darwin's initial apple-vector path entry changed after the iOS 4-era
// process ABI. Keep the choice in a named profile so ProcessLoader does not
// infer it from a firmware or application name.
enum class DarwinInitialAppleVectorAbi : std::uint8_t {
    KeyedExecutablePath,
    LegacyExecutablePath,
};

} // namespace ilemu
