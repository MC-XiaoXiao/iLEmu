// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "foundation/darwin_version.hpp"
#include "kernel/darwin_abi.hpp"

namespace ilemu {
// Native FMASK/FCNTLFLAGS and VFS open1 descriptor inheritance.
class DarwinFileFlags {
public:
    explicit constexpr DarwinFileFlags(DarwinVersion version)
        : version_ { version }
    {
    }

    [[nodiscard]] constexpr std::uint32_t mutable_status() const
    {
        using namespace darwin::open_flag;
        return append | non_block | asynchronous | synchronize |
               ((version_ < DarwinGeneration::IphoneOs3) ? 0U
                                                         : data_synchronize);
    }

    [[nodiscard]] constexpr std::uint32_t status(std::uint32_t flags, bool vnode = false) const
    {
        const auto mask = mutable_status() | darwin::open_flag::access_mode |
            (vnode ? darwin::open_flag::event_only : 0U);
        // Native FFLAGS/OFLAGS convert access modes to FREAD/FWRITE and back.
        return ((flags + 1U) & mask) - 1U;
    }

    [[nodiscard]] constexpr std::uint32_t descriptor(std::uint32_t flags) const
    {
        return (version_ >= DarwinGeneration::Ios4) &&
                       (flags & darwin::open_flag::close_on_exec) != 0U
                   ? 1U
                   : 0U;
    }

private:
    DarwinVersion version_;
};
} // namespace ilemu
