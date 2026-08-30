#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>

#include "ilemu/cpu.hpp"

namespace ilemu {

struct GuestExecutionRequest {
    Cpu* cpu { };
    std::size_t execution_slot { };
    std::uint64_t tick_budget { };
    std::chrono::nanoseconds host_slice_budget { };
    bool single_step { };
    CpuRunResult result;
    std::exception_ptr error;
};

// Owns host execution lanes for a batch selected by the Guest scheduler. The
// caller supplies policy results and remains responsible for applying Guest
// completion semantics; this class only executes independent CPU requests.
class GuestExecutionCoordinator {
public:
    explicit GuestExecutionCoordinator(std::size_t worker_count);
    ~GuestExecutionCoordinator();

    GuestExecutionCoordinator(const GuestExecutionCoordinator&) = delete;
    GuestExecutionCoordinator& operator=(
        const GuestExecutionCoordinator&) = delete;

    void run(std::span<GuestExecutionRequest*> requests);
    static void execute(GuestExecutionRequest& request) noexcept;

private:
    void worker_loop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ilemu
