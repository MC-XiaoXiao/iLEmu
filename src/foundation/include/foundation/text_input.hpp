// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <string>

namespace ilemu {
// Committed host text, independent of window systems and guest input methods.
struct TextInput {
    std::string utf8;
};
} // namespace ilemu
