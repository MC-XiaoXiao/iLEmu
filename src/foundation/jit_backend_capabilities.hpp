// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

namespace ilemu {

// Capabilities of the pinned Dynarmic backend, independent of guest firmware.
// Other backends use executor-local caches and the normal demand compiler.
struct JitBackendCapabilities {
#if defined(__x86_64__) || defined(_M_X64)
    static constexpr bool shared_native_cache = true;
    static constexpr bool portable_ir = true;
    static constexpr bool precompile = true;
    static constexpr bool dispatch_counters = true;
#else
#if defined(__aarch64__) || defined(_M_ARM64)
    static constexpr bool shared_native_cache = true;
    static constexpr bool portable_ir = true;
    static constexpr bool precompile = true;
#else
    static constexpr bool shared_native_cache = false;
    static constexpr bool portable_ir = false;
    static constexpr bool precompile = false;
#endif
    static constexpr bool dispatch_counters = false;
#endif
    // Runtime links allow guarded page-table views to change between entries.
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64)
    static constexpr bool runtime_memory_table_links = true;
#else
    static constexpr bool runtime_memory_table_links = false;
#endif
    // Run/Step, cold lookup and exclusive callbacks coordinate the native
    // lease with the existing checked-memory and reservation boundaries.
    static constexpr bool parallel_memory_leases = runtime_memory_table_links;
};

} // namespace ilemu
