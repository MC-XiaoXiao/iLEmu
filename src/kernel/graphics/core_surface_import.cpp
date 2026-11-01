// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/core_surface_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/core_surface_abi.hpp"
#include "surface_guest_import.hpp"

#include <utility>

namespace ilemu {
void CoreSurfaceHle::import_buffer(
    UserlandHleCall& call, Buffer& buffer, bool flushing)
try {
    const auto client = buffer.client;
    const auto identity = buffer.identity;
    const auto& abi = surface_transport::for_kind(buffer.transport);
    const auto client_mapping = call.memory().observe_mapping(
        buffer.client + abi.reference_count_offset, sizeof(std::uint32_t));
    const auto seed_output =
        !flushing && abi.lock_seed_output ? call.argument(2) : 0U;
    const auto seed_mapping = call.memory().observe_mapping(
        seed_output, seed_output ? sizeof(std::uint32_t) : 0U);
    auto options = call.argument(1);
    if (!flushing && !buffer.lock_options.empty()) {
        options = buffer.lock_options.back();
        // Match this Unlock exactly once before it can yield. Other threads
        // may acquire/release additional locks while its import is pending.
        buffer.lock_options.pop_back();
    }
    auto completion = [this, client, identity, client_mapping, seed_mapping,
                          seed_output, flushing](UserlandHleCall& resumed,
                          bool succeeded) {
        try {
            const auto found = buffers_.find(client);
            if (!succeeded || found == buffers_.end() ||
                found->second.identity != identity || !client_mapping.valid()) {
                resumed.set_return(1U);
                return;
            }
            auto& current = found->second;
            if (!flushing) {
                ++current.seed;
                submit(current, resumed);
                if (seed_output && seed_mapping.valid())
                    static_cast<void>(
                        resumed.memory().write32(seed_output, current.seed));
            }
            resumed.set_return(0U);
        } catch (...) {
            resumed.set_return(1U);
        }
    };
    if (!flushing && (options & core_surface_abi::lock_read_only) != 0) {
        // Read-only Unlock releases the lock without advancing its seed.
        if (seed_output && seed_mapping.valid())
            static_cast<void>(call.memory().write32(seed_output, buffer.seed));
        call.set_return(0U);
        return;
    }
    SurfaceGuestImport::run(
        call, surfaces_, buffer.id, std::move(completion));
} catch (...) {
    call.set_return(1U);
}
} // namespace ilemu
