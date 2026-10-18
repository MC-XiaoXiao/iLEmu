// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "crypto/host_uuid.hpp"

#if defined(_WIN32)
#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>
#include <windows.h>
#include <rpc.h>
#else
#include <uuid/uuid.h>
#endif

namespace ilemu {

std::string make_host_uuid()
{
#if defined(_WIN32)
    UUID identifier { };
    const auto status = UuidCreate(&identifier);
    if (status != RPC_S_OK && status != RPC_S_UUID_LOCAL_ONLY)
        throw std::runtime_error { "host UUID creation failed" };
    RPC_CSTR raw { };
    if (UuidToStringA(&identifier, &raw) != RPC_S_OK)
        throw std::runtime_error { "host UUID formatting failed" };
    const auto release = [](RPC_CSTR value) { RpcStringFreeA(&value); };
    const std::unique_ptr<unsigned char, decltype(release)> value { raw, release };
    std::string result { reinterpret_cast<const char*>(value.get()) };
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return result;
#else
    uuid_t identifier;
    char result[37] { };
    uuid_generate_random(identifier);
    uuid_unparse_upper(identifier, result);
    return result;
#endif
}

} // namespace ilemu
