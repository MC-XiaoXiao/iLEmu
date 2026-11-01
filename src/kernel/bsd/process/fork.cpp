// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/cpu.hpp"
#include "foundation/output.hpp"
#include "foundation/performance.hpp"
#include "kernel/kernel.hpp"

namespace ilemu {
void CompatibilityKernel::dispatch_fork(Cpu& cpu, bool vfork)
{
    if (!opengles_hle_.fork_preparation_required()) {
        complete_fork(cpu, vfork);
        return;
    }
    const auto saved = userland_hle_.run_kernel_operation(cpu, process_.pid,
        vfork ? "<vfork graphics>" : "<fork graphics>",
        [this, vfork](UserlandHleCall& call) {
            opengles_hle_.prepare_fork(
                call, [this, vfork](UserlandHleCall& ready, bool prepared) {
                    if (!prepared) {
                        bsd_error(ready.cpu(), 11); // EAGAIN
                        return;
                    }
                    complete_fork(ready.cpu(), vfork);
                });
        });
    if (!saved) {
        opengles_hle_.cancel_pending_work(cpu.processor_id());
        bsd_error(cpu, 11);
    }
}

void CompatibilityKernel::complete_fork(Cpu& cpu, bool vfork)
{
    const auto child = fork_handler_ ? fork_handler_(cpu) : std::nullopt;
    if (!child) {
        bsd_error(cpu, 11); // EAGAIN
        return;
    }
    performance_counters().record_fork();
    if (vfork) {
        // Preserve the existing private-address-space vfork model.
        const std::lock_guard mach_lock { shared_state_->mach_mutex };
        shared_state_->processes.at(*child).in_vfork = true;
    }
    output_.write(std::string { "[process] " } + (vfork ? "vfork" : "fork") +
                  " parent=" + std::to_string(process_.pid) +
                  " child=" + std::to_string(*child) + " bootstrap=" +
                  std::to_string(process_.bootstrap_port) + "\n");
    bsd_success(cpu, *child);
    if (scheduler_preemption_query_ &&
        scheduler_preemption_query_(cpu.processor_id())) {
        // Child publication precedes parent execution. Keep the existing
        // quantum and semantic-preemption policy at either return boundary.
        cpu.request_guest_preemption();
    }
}
} // namespace ilemu
