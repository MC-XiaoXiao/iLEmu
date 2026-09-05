#include "kernel/kernel.hpp"

#include <mutex>
#include <vector>

namespace ilemu {

std::vector<ProcessSnapshot> CompatibilityKernel::process_snapshots() const
{
    const std::lock_guard lock { shared_state_->mach_mutex };
    std::vector<ProcessSnapshot> snapshots;
    snapshots.reserve(shared_state_->processes.size());
    for (const auto& [pid, process] : shared_state_->processes) {
        snapshots.push_back(ProcessSnapshot { pid, process.parent_pid,
            process.command, process.executable_path, process.exited,
            process.pid_suspended, process.signal_stopped,
            process.exit_status, process.termination_signal });
    }
    return snapshots;
}

} // namespace ilemu
