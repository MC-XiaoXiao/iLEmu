// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/cpu.hpp"
#include "kernel/opengles_hle.hpp"

namespace ilemu {
bool OpenGlesHle::fork_preparation_required() const
{
    return pending_program_target_.has_value() || program_flush_failed_ ||
           (sync_lease_ && !sync_lease_->consumed);
}

void OpenGlesHle::prepare_fork(UserlandHleCall& call,
    std::function<void(UserlandHleCall&, bool)> completion)
{
    bool prepared = false;
    try {
        service_graphics_completions();
        if (program_flush_failed_) {
            prepared = false;
        } else if (sync_lease_ && !sync_lease_->consumed) {
            const auto lease = sync_lease_;
            call.suspend_until([lease] { return lease->ready(); },
                [this, completion](UserlandHleCall& resumed) {
                    prepare_fork(resumed, completion);
                },
                lease->result ? lease->result->sequence : 0);
            return;
        } else if (pending_program_target_) {
            await_program_flush(
                call, [this, completion](UserlandHleCall& resumed) {
                    prepare_fork(resumed, completion);
                });
            return;
        } else
            prepared = true;
    } catch (...) {
        // Failed admission/observation does not release accepted GPU work.
        // Other threads' existing leases keep their original observers.
        cancel_pending_work(call.cpu().processor_id());
    }
    // The kernel continuation may publish a child. Never call it a second
    // time if a later, non-graphics part of that publication throws.
    completion(call, prepared);
}
} // namespace ilemu
