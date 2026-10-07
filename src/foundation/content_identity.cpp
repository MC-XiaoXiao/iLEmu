// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Calculate and represent content hashes used to identify executable
// generations.

#include "foundation/content_identity.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>

#include <openssl/evp.h>

#include <unistd.h>

namespace ilemu {
namespace {

    class Sha256State {
    public:
        Sha256State()
            : context_ { EVP_MD_CTX_new(), EVP_MD_CTX_free }
        {
            if (!context_ ||
                EVP_DigestInit_ex(context_.get(), algorithm(), nullptr) != 1) {
                throw std::runtime_error { "SHA-256 initialization failed" };
            }
        }

        void update(std::span<const std::byte> bytes)
        {
            if (EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1)
                throw std::runtime_error { "SHA-256 update failed" };
        }

        [[nodiscard]] ContentIdentity finish()
        {
            ContentIdentity identity;
            unsigned size = 0;
            if (EVP_DigestFinal_ex(context_.get(),
                    reinterpret_cast<unsigned char*>(identity.digest.data()),
                    &size) != 1 || size != identity.digest.size()) {
                throw std::runtime_error { "SHA-256 finalization failed" };
            }
            return identity;
        }

    private:
        static const EVP_MD* algorithm()
        {
            // Share the immutable algorithm, while each stream owns its
            // context. OpenSSL selects the host's accelerated implementation.
            static const std::unique_ptr<EVP_MD, decltype(&EVP_MD_free)> digest {
                EVP_MD_fetch(nullptr, "SHA256", nullptr), EVP_MD_free
            };
            if (!digest)
                throw std::runtime_error { "SHA-256 implementation unavailable" };
            return digest.get();
        }

        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
    };

} // namespace

bool ContentIdentity::empty() const noexcept
{
    return std::all_of(digest.begin(), digest.end(),
        [](std::byte byte) { return byte == std::byte { 0 }; });
}

std::string ContentIdentity::hex() const
{
    constexpr std::array<char, 16> digits { '0', '1', '2', '3', '4', '5', '6',
        '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        const auto value = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0fU]);
    }
    return result;
}

std::size_t ContentIdentityHash::operator()(
    const ContentIdentity& identity) const noexcept
{
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(identity.digest[index]))
                 << static_cast<unsigned>(index * 8U);
    }
    return std::hash<std::uint64_t> { }(value);
}

ContentIdentity sha256(std::span<const std::byte> bytes)
{
    Sha256State state;
    state.update(bytes);
    return state.finish();
}

std::optional<ContentIdentity> sha256_file(int descriptor,
    std::uint64_t file_offset, std::optional<std::uint64_t> byte_count,
    const std::function<bool()>& cancellation_check)
{
    if (descriptor < 0 ||
        file_offset >
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::nullopt;
    }

    Sha256State state;
    std::array<std::byte, 64U * 1024U> buffer { };
    std::uint64_t current_offset = file_offset;
    auto remaining = byte_count;
    while (!remaining || *remaining != 0U) {
        if (cancellation_check && cancellation_check())
            return std::nullopt;
        const auto requested =
            remaining ? std::min<std::uint64_t>(*remaining, buffer.size())
                      : buffer.size();
        ssize_t count = -1;
        do {
            count = ::pread(descriptor, buffer.data(), requested,
                static_cast<off_t>(current_offset));
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            return std::nullopt;
        if (count == 0) {
            if (remaining && *remaining != 0U)
                return std::nullopt;
            break;
        }

        state.update(std::span<const std::byte> {
            buffer.data(), static_cast<std::size_t>(count) });
        const auto received = static_cast<std::uint64_t>(count);
        if (current_offset >
            std::numeric_limits<std::uint64_t>::max() - received) {
            return std::nullopt;
        }
        current_offset += received;
        if (remaining)
            *remaining -= received;
    }
    return state.finish();
}

std::optional<ContentIdentity> sha256_file(const std::filesystem::path& path,
    std::uint64_t file_offset, std::optional<std::uint64_t> byte_count,
    const std::function<bool()>& cancellation_check)
{
    std::ifstream input { path, std::ios::binary };
    if (!input)
        return std::nullopt;
    if (file_offset > static_cast<std::uint64_t>(
                          std::numeric_limits<std::streamoff>::max())) {
        return std::nullopt;
    }
    input.seekg(static_cast<std::streamoff>(file_offset));
    if (!input)
        return std::nullopt;

    Sha256State state;
    std::array<char, 64U * 1024U> buffer { };
    auto remaining = byte_count;
    while (!remaining || *remaining != 0U) {
        if (cancellation_check && cancellation_check())
            return std::nullopt;
        const auto requested =
            remaining ? std::min<std::uint64_t>(*remaining, buffer.size())
                      : buffer.size();
        input.read(buffer.data(), static_cast<std::streamsize>(requested));
        const auto count = input.gcount();
        if (count <= 0) {
            if (input.eof() && (!remaining || *remaining == 0U))
                break;
            return std::nullopt;
        }
        const auto bytes = std::span<const std::byte> {
            reinterpret_cast<const std::byte*>(buffer.data()),
            static_cast<std::size_t>(count)
        };
        state.update(bytes);
        if (remaining) {
            *remaining -= static_cast<std::uint64_t>(count);
        }
        if (input.bad())
            return std::nullopt;
        if (input.eof()) {
            if (remaining && *remaining != 0U)
                return std::nullopt;
            break;
        }
        if (!input)
            return std::nullopt;
    }
    return state.finish();
}

} // namespace ilemu
