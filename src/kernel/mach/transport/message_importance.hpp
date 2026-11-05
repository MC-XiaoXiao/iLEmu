/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../support.hpp"
#include "kernel/darwin_abi.hpp"
#include "mach/mig_wire_abi.hpp"
#include "kernel/kernel_shared_state.hpp"

namespace ilemu::mach_transport {

// These operations share the transport's mach_mutex transaction. The model's
// importance lock never calls back into the kernel or namespace tables.
inline void copyin_message_importance(KernelSharedState& state,
    const ProcessContext& sender, KernelSharedState::MachMessage& message,
    std::uint32_t options)
{
    constexpr std::uint32_t no_importance = 0x00040000U;
    if ((options & no_importance) != 0U)
        return;
    const auto port = state.mach_port_objects.lookup(message.destination);
    const auto& task = sender.execution_policy->importance();
    if (!port || !port->importance_receiver || task->never_donor())
        return;
    std::shared_ptr<xnu::ipc::ImportanceElement> source = task;
    if (task->receiver() && message.voucher_object) {
        const auto voucher = state.mach_vouchers.find(*message.voucher_object);
        if (voucher != state.mach_vouchers.end()) {
            const auto value = voucher->second.importance();
            const auto inherit =
                std::dynamic_pointer_cast<xnu::ipc::ImportanceAssertion>(value);
            if (value == task ||
                (inherit && inherit->inheritance() && inherit->target() == task))
                source = value;
        }
    }
    message.importance = xnu::ipc::ImportanceAssertion::create(
        port->importance, std::move(source), task->donating());
}

// Port donation is transferred before right copyout. A failed body or buffer
// copyout drops just this receive's external reference, even when its voucher
// value is shared with an earlier successful receive.
class ReceiveImportance {
public:
    ReceiveImportance(KernelSharedState& state, const ProcessContext& receiver,
        KernelSharedState::MachMessage& message, std::uint32_t options,
        std::vector<std::byte>& bytes)
        : task_ { receiver.execution_policy->importance() }
        , message_ { message.importance }
    {
        constexpr std::uint32_t raised_importance = 0x20000000U;
        auto bits = mach_support::read_little_word(bytes, 0U) & ~raised_importance;
        if ((options & darwin::mach_message::option_receive_voucher) != 0U) {
            if (message_)
                inheritance_ = xnu::ipc::ImportanceAssertion::inherit(
                    task_, message_->source(), message_->donating());
            if (inheritance_)
                value_ = xnu::ipc::ImportanceValue::create(inheritance_);
            if (message.voucher_object || inheritance_) {
                MachVoucherState value;
                if (message.voucher_object)
                    value = state.mach_vouchers.at(*message.voucher_object);
                value.set_importance(inheritance_);
                const bool has_value = !value.empty();
                const auto interned = state.intern_mach_voucher_locked(std::move(value));
                if (has_value && interned.object == 0U) {
                    error_ = darwin::mach_message::receive_invalid_data;
                    return;
                }
                if (interned.object != 0U)
                    ++state.mach_inflight_send_rights[interned.object];
                if (message.voucher_object)
                    mach_support::release_inflight_send_right_locked(state, *message.voucher_object);
                message.voucher_object = interned.object != 0U
                    ? std::optional { interned.object } : std::nullopt;
                message.voucher_right = interned.object != 0U
                    ? std::optional { xnu::ipc::Right::Send } : std::nullopt;
                bits = (bits & ~0x001f0000U) |
                       (interned.object != 0U ? 17U << 16U : 0U);
                mach_support::write_little_word(bytes,
                    darwin::mig_wire::header_voucher_offset, 0U);
            }
            if (inheritance_)
                bits |= raised_importance;
        } else if (message_ && message_->donating() && task_->accepts()) {
            message_->retarget(task_);
            legacy_ = message_->externalize_legacy();
            if (legacy_)
                bits |= raised_importance;
        }
        if (message_)
            message_->cancel();
        mach_support::write_little_word(bytes, 0U, bits);
    }

    ReceiveImportance(const ReceiveImportance&) = delete;
    ReceiveImportance& operator=(const ReceiveImportance&) = delete;

    ~ReceiveImportance()
    {
        if (committed_)
            return;
        if (inheritance_)
            inheritance_->undo_receive_external();
        if (legacy_)
            static_cast<void>(task_->drop_legacy());
        if (message_)
            message_->cancel();
    }
    [[nodiscard]] std::uint32_t error() const { return error_; }
    void commit() { committed_ = true; }

private:
    std::shared_ptr<xnu::ipc::TaskImportance> task_;
    std::shared_ptr<xnu::ipc::ImportanceAssertion> message_;
    std::shared_ptr<xnu::ipc::ImportanceAssertion> inheritance_;
    std::shared_ptr<xnu::ipc::ImportanceValue> value_;
    bool legacy_ { };
    bool committed_ { };
    std::uint32_t error_ { };
};

} // namespace ilemu::mach_transport
