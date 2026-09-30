// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <network/darwin_abi_route.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace ilemu {

// Effective gid remains authoritative in ProcessContext; this owns its tail.
class CredentialGroups {
public:
    static constexpr std::uint32_t maximum_count = 16U;
    static constexpr std::uint32_t get_syscall = 79U;
    [[nodiscard]] std::uint32_t count() const { return size_ + 1U; }
    [[nodiscard]] std::uint32_t at(
        std::uint32_t index, std::uint32_t effective_gid) const
    {
        return index == 0U ? effective_gid : supplementary_[index - 1U];
    }
    // Caller validates NGROUPS and installs the first entry as effective_gid.
    void assign(std::span<const std::uint32_t> groups)
    {
        size_ = groups.empty() ? 0U : static_cast<std::uint32_t>(groups.size() - 1U);
        supplementary_.fill(0U);
        if (size_ != 0U)
            std::copy_n(groups.begin() + 1, size_, supplementary_.begin());
    }
    void change_effective(std::uint32_t old_gid, std::uint32_t new_gid,
        DarwinAbiEpoch epoch)
    {
        // xnu-792 replaces group 0. xnu-1228+ preserves a matching member
        // by swapping it with the old effective gid (kauth_cred_change_egid).
        if (epoch == DarwinAbiEpoch::IphoneOs1)
            return;
        for (std::uint32_t i = 0; i < size_; ++i) {
            if (supplementary_[i] == new_gid) {
                supplementary_[i] = old_gid;
                break;
            }
        }
    }
private:
    std::array<std::uint32_t, maximum_count - 1U> supplementary_ { };
    std::uint32_t size_ { };
};

} // namespace ilemu
