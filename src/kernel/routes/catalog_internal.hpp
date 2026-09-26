// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "kernel/syscall_routes.hpp"
namespace ilemu::syscall_routes {
void bind_bsd_entries(Table& table, const DarwinAbi& abi);
void bind_pthread_entries(Table& table, const DarwinAbi& abi);
void bind_mach_entries(Table& table, const DarwinAbi& abi);
void bind_alias_entries(Table& table);
}
