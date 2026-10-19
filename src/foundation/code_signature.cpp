// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "foundation/code_signature.hpp"

#include <algorithm>
#include <limits>
#include <openssl/evp.h>

namespace ilemu {
namespace {
    std::uint32_t be32(std::span<const std::byte> bytes, std::size_t offset)
    {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < 4; ++i)
            result =
                (result << 8) | std::to_integer<unsigned>(bytes[offset + i]);
        return result;
    }

    std::uint64_t be64(std::span<const std::byte> bytes, std::size_t offset)
    {
        return (static_cast<std::uint64_t>(be32(bytes, offset)) << 32U) |
               be32(bytes, offset + 4U);
    }

    bool digest_matches(std::span<const std::byte> bytes,
        std::span<const std::byte> expected, const EVP_MD* algorithm)
    {
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest { };
        unsigned size = 0;
        return EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size,
                   algorithm, nullptr) == 1 &&
               expected.size() <= size &&
               std::equal(expected.begin(), expected.end(),
                   reinterpret_cast<const std::byte*>(digest.data()));
    }
}

std::optional<std::uint64_t> CodeSignature::covered_end_offset(
    std::span<const std::byte> signature)
{
    if (signature.size() < 12U || be32(signature, 0U) != 0xfade0cc0U)
        return std::nullopt;
    const auto length = be32(signature, 4U);
    const auto count = be32(signature, 8U);
    if (length < 12U || length > signature.size() ||
        count > (length - 12U) / 8U)
        return std::nullopt;
    signature = signature.first(length);
    for (std::size_t i = 0U; i < count; ++i) {
        if (be32(signature, 12U + i * 8U) != 0U)
            continue;
        const auto offset = be32(signature, 16U + i * 8U);
        if (offset > length || length - offset < 44U)
            return std::nullopt;
        const auto directory = signature.subspan(offset);
        const auto directory_size = be32(directory, 4U);
        if (be32(directory, 0U) != 0xfade0c02U ||
            directory_size < 44U || directory_size > directory.size())
            return std::nullopt;
        const auto version = be32(directory, 8U);
        const auto hash_offset = be32(directory, 16U);
        const auto code_slots = be32(directory, 28U);
        auto code_limit = static_cast<std::uint64_t>(be32(directory, 32U));
        const auto hash_size = std::to_integer<unsigned>(directory[36]);
        const auto page_shift = std::to_integer<unsigned>(directory[39]);
        if (version >= 0x20300U && code_limit == 0xffffffffU) {
            if (directory_size < 64U)
                return std::nullopt;
            code_limit = be64(directory, 56U);
        }
        if (code_limit == 0U || hash_size == 0U || page_shift > 30U ||
            hash_offset > directory_size ||
            code_slots > (directory_size - hash_offset) / hash_size)
            return std::nullopt;
        if (version >= 0x20100U) {
            if (directory_size < 48U)
                return std::nullopt;
            const auto scatter_offset = be32(directory, 44U);
            if (scatter_offset != 0U) {
                constexpr std::size_t scatter_entry_size = 24U;
                if (scatter_offset > directory_size ||
                    directory_size - scatter_offset < scatter_entry_size)
                    return std::nullopt;
                bool terminated = false;
                std::uint32_t scattered_pages = 0U;
                for (std::size_t entry = scatter_offset;
                     entry <= directory_size - scatter_entry_size;
                     entry += scatter_entry_size) {
                    const auto count = be32(directory, entry);
                    if (count == 0U) {
                        terminated = true;
                        break;
                    }
                    if (count > code_slots - scattered_pages)
                        return std::nullopt;
                    scattered_pages += count;
                }
                if (!terminated)
                    return std::nullopt;
            }
        }
        const auto page_size = std::uint64_t { 1 } << page_shift;
        if (code_limit > std::numeric_limits<std::uint64_t>::max() -
                             (page_size - 1U))
            return std::nullopt;
        if (code_slots != (code_limit + page_size - 1U) / page_size)
            return std::nullopt;
        return (code_limit + page_size - 1U) & ~(page_size - 1U);
    }
    return std::nullopt;
}

std::optional<CodeSignature> CodeSignature::inspect(
    std::span<const std::byte> image, std::span<const std::byte> signature)
{
    if (signature.size() < 12 || be32(signature, 0) != 0xfade0cc0U)
        return std::nullopt;
    const auto length = be32(signature, 4);
    const auto count = be32(signature, 8);
    if (length < 12 || length > signature.size() || count > (length - 12) / 8)
        return std::nullopt;
    signature = signature.first(length);
    std::span<const std::byte> directory;
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = be32(signature, 16 + i * 8);
        if (offset > length || length - offset < 8)
            return std::nullopt;
        const auto size = be32(signature, offset + 4);
        if (size < 8 || size > length - offset)
            return std::nullopt;
        if (be32(signature, 12 + i * 8) == 0)
            directory = signature.subspan(offset, size);
    }
    if (directory.size() < 44 || be32(directory, 0) != 0xfade0c02U)
        return std::nullopt;
    const auto version = be32(directory, 8);
    const auto hash_offset = be32(directory, 16);
    const auto special_count = be32(directory, 24);
    const auto page_count = be32(directory, 28);
    const auto code_limit = be32(directory, 32);
    const auto hash_size = std::to_integer<unsigned>(directory[36]);
    const auto hash_type = std::to_integer<unsigned>(directory[37]);
    const auto page_shift = std::to_integer<unsigned>(directory[39]);
    const auto* algorithm = hash_type == 1                       ? EVP_sha1()
                            : (hash_type == 2 || hash_type == 3) ? EVP_sha256()
                                                                 : nullptr;
    const auto expected_size = hash_type == 2 ? 32U : 20U;
    // Scatter directories need a different page mapping; never treat them as
    // a contiguous signature. The extended 64-bit code limit is unnecessary
    // for this 32-bit loader and is likewise rejected when selected.
    if (!algorithm || hash_size != expected_size || page_shift > 30 ||
        code_limit == 0 || code_limit > image.size() ||
        (version >= 0x20100 &&
            (directory.size() < 48 || be32(directory, 44) != 0)) ||
        code_limit == 0xffffffffU || hash_offset > directory.size() ||
        special_count > hash_offset / hash_size ||
        page_count > (directory.size() - hash_offset) / hash_size)
        return std::nullopt;
    const std::size_t page_size =
        page_shift == 0 ? code_limit : std::size_t { 1 } << page_shift;
    if (page_count != (code_limit + page_size - 1) / page_size)
        return std::nullopt;
    for (std::size_t page = 0; page < page_count; ++page) {
        const auto offset = page * page_size;
        if (!digest_matches(
                image.subspan(offset, std::min(page_size, code_limit - offset)),
                directory.subspan(hash_offset + page * hash_size, hash_size),
                algorithm))
            return std::nullopt;
    }
    // Embedded special slots (requirements, entitlements) are covered by the
    // directory too. External bundle resources are not executable pages.
    for (std::size_t i = 0; i < count; ++i) {
        const auto slot = be32(signature, 12 + i * 8);
        if (slot == 0 || slot > special_count)
            continue;
        const auto offset = be32(signature, 16 + i * 8);
        if (!digest_matches(
                signature.subspan(offset, be32(signature, offset + 4)),
                directory.subspan(hash_offset - slot * hash_size, hash_size),
                algorithm))
            return std::nullopt;
    }
    CodeSignature result {
        .hash = { }, .flags = be32(directory, 12), .identifier = { }
    };
    const auto identifier_offset = be32(directory, 20);
    if (identifier_offset < directory.size()) {
        const auto identifier = directory.subspan(identifier_offset);
        const auto end = std::find(identifier.begin(), identifier.end(), std::byte { 0 });
        if (end != identifier.end())
            result.identifier.assign(
                reinterpret_cast<const char*>(identifier.data()),
                static_cast<std::size_t>(end - identifier.begin()));
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest { };
    unsigned size = 0;
    if (EVP_Digest(directory.data(), directory.size(), digest.data(), &size,
            algorithm, nullptr) != 1 ||
        size < result.hash.size())
        return std::nullopt;
    std::copy_n(reinterpret_cast<const std::byte*>(digest.data()),
        result.hash.size(), result.hash.begin());
    return result;
}

PlatformTrustCache PlatformTrustCache::from_kernel(
    std::span<const std::byte> kernel)
{
    PlatformTrustCache result;
    constexpr std::size_t bucket_bytes = 256 * 4;
    const auto word = [&](std::size_t offset) {
        return std::to_integer<unsigned>(kernel[offset]) |
               (std::to_integer<unsigned>(kernel[offset + 1]) << 8);
    };
    // Only accept a complete, unambiguous table. Contiguous bucket indices,
    // bounds and strict sorting distinguish the cache from unrelated data.
    for (std::size_t base = 0; base + bucket_bytes <= kernel.size();
        base += 2) {
        if (word(base) == 0 || word(base + 2) != 0)
            continue;
        std::size_t total = 0;
        bool valid = true;
        for (unsigned bucket = 0; bucket < 256; ++bucket) {
            if (word(base + bucket * 4 + 2) != total) {
                valid = false;
                break;
            }
            total += word(base + bucket * 4);
        }
        const auto data = base + bucket_bytes;
        if (!valid || total == 0 || total > (kernel.size() - data) / 19)
            continue;
        std::vector<CodeDirectoryHash> hashes;
        hashes.reserve(total);
        std::size_t index = 0;
        for (unsigned bucket = 0; bucket < 256 && valid; ++bucket) {
            for (unsigned j = 0; j < word(base + bucket * 4); ++j, ++index) {
                CodeDirectoryHash hash;
                hash[0] = static_cast<std::byte>(bucket);
                std::copy_n(
                    kernel.subspan(data + index * 19, 19).begin(), 19, hash.begin() + 1);
                if (!hashes.empty() && hash <= hashes.back()) {
                    valid = false;
                    break;
                }
                hashes.push_back(hash);
            }
        }
        if (!valid)
            continue;
        if (!result.hashes_.empty())
            return { }; // Ambiguity must not grant platform trust.
        result.hashes_ = std::move(hashes);
        base = data + total * 19 - 2;
        base -= base % 2;
    }
    return result;
}

bool PlatformTrustCache::contains(const CodeDirectoryHash& hash) const
{
    return std::binary_search(hashes_.begin(), hashes_.end(), hash);
}

} // namespace ilemu
