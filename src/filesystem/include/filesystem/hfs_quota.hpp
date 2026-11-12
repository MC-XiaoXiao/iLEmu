// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace ilemu::hfs {

// XNU quota.h: ARM32 dqblk is 64 bytes; quota files use big endian fields.
using QuotaRecord = std::array<std::byte, 64>;
inline constexpr std::uint64_t quota_allocation(std::uint64_t bytes)
{
    return (bytes + 4095U) / 4096U * 4096U;
}

class Quotas {
public:
    Quotas();
    [[nodiscard]] bool active(std::string_view mount) const;
    [[nodiscard]] std::uint32_t control(std::string_view mount,
        std::uint32_t command, std::uint32_t type, std::uint32_t id,
        QuotaRecord& record, const std::filesystem::path& file,
        std::uint32_t now);
    // Both user and group charges are checked before either is committed.
    [[nodiscard]] std::uint32_t change(std::string_view mount,
        std::uint32_t owner, std::uint32_t group, std::int64_t bytes,
        std::int64_t inodes, bool privileged, std::uint32_t now);

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace ilemu::hfs
