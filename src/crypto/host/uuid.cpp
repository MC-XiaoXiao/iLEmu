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
#elif defined(__ANDROID__)
#include <openssl/rand.h>
#include <array>
#include <stdexcept>
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
#elif defined(__ANDROID__)
    // Bionic has no libuuid. Use the existing cryptographic provider for the
    // RFC 9562 version-4 layout rather than linking the build host's libuuid.
    std::array<unsigned char, 16> bytes {};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
        throw std::runtime_error { "host UUID entropy unavailable" };
    bytes[6] = (bytes[6] & 0x0fU) | 0x40U;
    bytes[8] = (bytes[8] & 0x3fU) | 0x80U;
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            result += '-';
        result += digits[bytes[index] >> 4U];
        result += digits[bytes[index] & 0x0fU];
    }
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
