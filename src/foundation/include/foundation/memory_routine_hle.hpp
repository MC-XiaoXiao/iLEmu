// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/arm_cpu_model.hpp"

namespace ilemu {
class UserlandHleRegistry;
void register_memory_routine_hle(
    UserlandHleRegistry& registry, ArmArchitectureVersion architecture);
} // namespace ilemu
