// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>

namespace ilemu {

class UserlandHleCall;

// The drawable's nativeWindow is a firmware-owned function table. Keep its
// allocation and buffer rotation in QuartzCore; GLES only supplies pixels.
class GuestEaglWindow {
public:
    using StorageCompletion =
        std::function<void(UserlandHleCall&, std::uint32_t)>;
    using PresentCompletion =
        std::function<void(UserlandHleCall&, bool, std::uint32_t)>;

    [[nodiscard]] static std::optional<GuestEaglWindow> open(
        UserlandHleCall& call, std::uint32_t address);

    void configure(UserlandHleCall& call, StorageCompletion completion) const;
    void present(UserlandHleCall& call, PresentCompletion completion) const;

    [[nodiscard]] std::uint32_t address() const { return address_; }

private:
    explicit GuestEaglWindow(std::uint32_t address) : address_ { address } { }

    void acquire(UserlandHleCall& call, StorageCompletion completion) const;

    std::uint32_t address_ { };
};

} // namespace ilemu
