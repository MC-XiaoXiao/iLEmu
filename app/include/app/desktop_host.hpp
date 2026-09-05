#pragma once

#include "runtime/session_host.hpp"

namespace ilemu {

// The CLI's composition root connects optional native adapters to a session.
class DesktopHost final : public SessionHost {
public:
    void initialize_graphics() override;
    [[nodiscard]] std::unique_ptr<DisplayPresenter> create_display(
        const DeviceModel& device) override;
    [[nodiscard]] std::unique_ptr<ControlChannel> create_control(
        const DeviceModel& device) override;
    [[nodiscard]] SessionAudio create_audio() override;
    [[nodiscard]] HostMemorySnapshot memory_snapshot() const override;
    [[nodiscard]] HostMemoryBudgetSnapshot
    memory_budget_snapshot() const override;
};

} // namespace ilemu
