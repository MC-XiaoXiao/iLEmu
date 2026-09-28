// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "../support.hpp"
#include <algorithm>

namespace ilemu::mach_transport {

// After atomic header copyin, body errors destroy already copied rights.
// Hold mach_mutex for this scope, including its destructor.
class CopyinCleanup {
public:
    CopyinCleanup(KernelSharedState& state,
        KernelSharedState::MachMessage& message,
        std::optional<std::uint32_t> destination_send_once)
        : state_(state), message_(message),
          destination_send_once_(destination_send_once)
    {
    }
    CopyinCleanup(const CopyinCleanup&) = delete;
    CopyinCleanup& operator=(const CopyinCleanup&) = delete;
    ~CopyinCleanup() { discard(); }
    void discard()
    {
        if (committed_)
            return;
        committed_ = true;
        // ipc_kmsg_copyin_ool_ports_descriptor destroys a failed array's
        // prefix before ipc_kmsg_clean_partial cleans the header and body.
        if (failed_array_) {
            for (auto& transfer : message_.port_transfers) {
                if (transfer.descriptor_offset != *failed_array_)
                    continue;
                KernelSharedState::MachMessage element;
                element.reply_object = transfer.object;
                element.reply_right = transfer.right;
                mach_support::discard_mach_message_rights_locked(state_, element);
                transfer.right = xnu::ipc::Right::DeadName;
            }
        }
        if (destination_send_once_) {
            mach_support::enqueue_send_once_notification_locked(
                state_, *destination_send_once_);
            message_.destination_send_once_object.reset();
        }
        if (message_.destination_send_object) {
            mach_support::release_inflight_send_right_locked(
                state_, *message_.destination_send_object);
            message_.destination_send_object.reset();
        }
        // Compact-descriptor copyin walks backwards; native cleanup still
        // walks the completed descriptors in their physical message order.
        std::sort(message_.port_transfers.begin(), message_.port_transfers.end(),
            [](const auto& left, const auto& right) {
                if (left.descriptor_offset != right.descriptor_offset)
                    return left.descriptor_offset < right.descriptor_offset;
                return left.array_index < right.array_index;
            });
        mach_support::discard_mach_message_rights_locked(state_, message_);
    }
    void failed_array(std::uint32_t offset) { failed_array_ = offset; }
    void commit() { committed_ = true; }
private:
    KernelSharedState& state_;
    KernelSharedState::MachMessage& message_;
    std::optional<std::uint32_t> destination_send_once_;
    std::optional<std::uint32_t> failed_array_;
    bool committed_ { };
};

} // namespace ilemu::mach_transport
