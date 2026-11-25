// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>

namespace ilemu {

class UserlandHleCall;

void trace_native_io_entry(UserlandHleCall& call);
void set_native_io_result(UserlandHleCall& call, std::uint32_t result);

} // namespace ilemu
