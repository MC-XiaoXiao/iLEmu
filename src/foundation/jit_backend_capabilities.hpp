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
    static constexpr bool shared_native_cache = false;
    static constexpr bool portable_ir = false;
    static constexpr bool precompile = false;
    static constexpr bool dispatch_counters = false;
#endif
};

} // namespace ilemu
