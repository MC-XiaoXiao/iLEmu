// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"

namespace ilemu {

void CompatibilityKernel::notify_child_status(const ChildStatus& status)
{
    using namespace darwin::signal;
    if (process_.exited)
        return;
    const auto& action = signal_actions_[child];
    if (status.kind == ChildStatus::Kind::Stop) {
        if (action[3] & no_child_stop_flag)
            return;
        signal_sender_ = { status.pid, status.uid,
            signal_state_.child_stop_status(status.status), 5U }; // CLD_STOPPED
    } else {
        // Explicit SIG_IGN and SA_NOCLDWAIT both set P_NOCLDWAIT in XNU.
        // SIG_DFL merely suppresses the notification and retains the zombie.
        if (action[0] == ignore_action || (action[3] & no_child_wait_flag)) {
            bool reparented = false;
            {
                std::lock_guard lock { shared_state_->mach_mutex };
                const auto record = shared_state_->processes.find(status.pid);
                if (record == shared_state_->processes.end() ||
                    record->second.parent_pid != process_.pid || !record->second.exited)
                    return;
                if (!signal_state_.kernel_reaps_ignored_children() && process_.pid != 1U) {
                    record->second.parent_pid = 1;
                    const auto init = shared_state_->processes.find(1);
                    record->second.parent_incarnation = init == shared_state_->processes.end()
                        ? 0U : init->second.incarnation;
                    if (init != shared_state_->processes.end())
                        ++init->second.child_wait_generation;
                    reparented = true;
                } else {
                    shared_state_->processes.erase(record);
                }
            }
            // XNU 792 reparents to init; 1228+ reaps internally. Neither
            // path posts SIGCHLD back to the parent requesting no zombies.
            if (reparented && child_status_handler_)
                child_status_handler_(1, status);
            shared_state_->note_io_event_transition();
            return;
        }
        // kern_exit.c does not publish child siginfo on initproc.
        if (process_.pid != 1U) {
            const auto signal = status.status & 0x7fU;
            signal_sender_ = { status.pid, status.uid,
                signal ? signal : (status.status >> 8U) & 0xffU,
                signal ? 2U : 1U }; // CLD_KILLED / CLD_EXITED
        }
    }
    // Parent metadata is recorded even when SIGCHLD is held. psignal's
    // ordinary sender path must not overwrite this child status record.
    static_cast<void>(deliver_signal(child));
}

} // namespace ilemu
