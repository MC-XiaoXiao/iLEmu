// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/display_geometry.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace ilemu {

class DarwinVersion;
struct ConsoleProgressImageProfile;

// The mobile framebuffer returns the panel to XNU's graphical console when
// its last type-zero user client closes. Callers serialize with mach_mutex.
class DisplayConsole {
public:
    void configure(DarwinVersion version);
    void acquire(std::uint64_t now);
    void open(std::uint32_t connection);
    void close(std::uint32_t connection, std::uint64_t now);
    [[nodiscard]] std::optional<std::uint64_t> next_deadline() const;
    [[nodiscard]] std::optional<std::vector<std::uint32_t>> take_frame(
        std::uint64_t now, DisplayGeometry panel, DisplayGeometry logical,
        bool powered_on);

private:
    const ConsoleProgressImageProfile* profile_ { };
    std::set<std::uint32_t> clients_;
    std::uint64_t started_at_ { };
    std::optional<std::uint64_t> deadline_;
};

} // namespace ilemu
