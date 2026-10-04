// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ilemu {

using CodeDirectoryHash = std::array<std::byte, 20>;

struct CodeSignature {
    CodeDirectoryHash hash;
    std::uint32_t flags;
    std::string identifier;
    // Validates the embedded directory and its executable page hashes.
    // Trust is determined separately by the firmware's trust cache.
    [[nodiscard]] static std::optional<CodeSignature> inspect(
        std::span<const std::byte> image, std::span<const std::byte> signature);
};

class PlatformTrustCache {
public:
    // Legacy AMFI embeds 256 count/index buckets followed by sorted 19-byte
    // SHA1 suffixes. The leading byte is implicit in the bucket number.
    [[nodiscard]] static PlatformTrustCache from_kernel(
        std::span<const std::byte> kernel);
    [[nodiscard]] bool contains(const CodeDirectoryHash& hash) const;
    [[nodiscard]] std::size_t size() const { return hashes_.size(); }

private:
    std::vector<CodeDirectoryHash> hashes_;
};

} // namespace ilemu
