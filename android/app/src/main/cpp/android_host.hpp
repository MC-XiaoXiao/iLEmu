// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "runtime/session_host.hpp"
#include <atomic>
#include <filesystem>

namespace ilemu {

class AndroidHost final : public SessionHost {
public:
    AndroidHost(
        std::filesystem::path control_path, const std::atomic<bool>& stop);
    ~AndroidHost() override;
    AndroidHost(const AndroidHost&) = delete;
    AndroidHost& operator=(const AndroidHost&) = delete;

    bool stop_requested() const override;
    void initialize_graphics() override;
    std::unique_ptr<DisplayPresenter> create_display(
        const DeviceModel&) override;
    std::unique_ptr<ControlChannel> create_control(const DeviceModel&) override;
    SessionAudio create_audio() override;
    HostMemorySnapshot memory_snapshot() const override;
    HostMemoryBudgetSnapshot memory_budget_snapshot() const override;

private:
    std::filesystem::path control_path_;
    const std::atomic<bool>& stop_;
    int control_fd_ { -1 };
};

} // namespace ilemu
