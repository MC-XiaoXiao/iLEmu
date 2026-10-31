// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>

namespace ilemu {
struct DisplayFrame;
class GlesRenderer;
class Output;

// Core-owned host file requests. Graphics work uses the existing bounded
// renderer queue; only poll publishes files and command completion markers.
class SessionSnapshots final {
public:
    static constexpr std::size_t maximum_pending = 64;
    static constexpr std::size_t maximum_scheduled = 1000;
    SessionSnapshots(std::shared_ptr<GlesRenderer> renderer, Output& output);
    ~SessionSnapshots();
    void submit(const std::filesystem::path& path, DisplayFrame frame,
        bool sequence = false);
    void poll();
    void cancel();
    [[nodiscard]] bool empty() const noexcept { return pending_.empty(); }

private:
    struct Pending;
    void publish(const Pending& request, const DisplayFrame& frame);
    void fail(const std::filesystem::path& path, const char* reason);
    std::shared_ptr<GlesRenderer> renderer_;
    Output& output_;
    std::deque<std::unique_ptr<Pending>> pending_;
};
} // namespace ilemu
