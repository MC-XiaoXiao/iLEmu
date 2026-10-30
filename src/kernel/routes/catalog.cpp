// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "catalog_internal.hpp"
#include <stdexcept>

namespace ilemu::syscall_routes {
Table build(DarwinVersion version)
{
    if (!version.known())
        throw std::invalid_argument("unsupported Darwin generation");
    Table table;
    bind_bsd_entries(table, version);
    bind_pthread_entries(table, version);
    bind_mach_entries(table, version);
    bind_alias_entries(table);
    table.validate();
    return table;
}
} // namespace ilemu::syscall_routes
