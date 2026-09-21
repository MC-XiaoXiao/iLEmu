// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Adapt guest IOHIDEventSystem calls to emulator input services.

#include "kernel/hid_event_system_hle.hpp"
#include "hid_event_transaction.hpp"
#include "hid_switch_query.hpp"

#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"
#include "kernel/kernel_shared_state.hpp"

#include <algorithm>
#include <string>

namespace ilemu {

HidEventSystemHle::HidEventSystemHle(UserlandHleRegistry& registry)
    : registry_ { registry }
{
    register_hid_switch_queries(registry_);
    registry_.register_function(
        "/IOKit", "_IOHIDEventSystemOpen", [this](UserlandHleCall& call) {
            HidEventQueue::Consumer consumer { call.process_id(),
                call.cpu().processor_id(), call.argument(0) };
            consumer.keyboard_events =
                call.symbol_address("_IOHIDEventCreateKeyboardEvent")
                    .has_value();
            call.resume_original_persistently([this, consumer](
                                                  UserlandHleCall& completed) {
                // Opening the event system establishes the hardware event
                // consumer even when its callback is installed later.
                // Native dispatch owns callback and client routing.
                if (!state_ || completed.argument(0) == 0U ||
                    !state_->user_interface_geometry.valid())
                    return;
                for (const auto symbol : { "_IOHIDEventCreateDigitizerEvent",
                         "_IOHIDEventAppendEvent",
                         "__IOHIDEventSystemDispatchEvent", "_CFRelease" }) {
                    if (!completed.symbol_address(symbol)) {
                        return;
                    }
                }
                consumer_process_ = consumer.process;
                consumer_processor_ = consumer.processor;
                state_->hid_event_queue.open(consumer);
                accelerometer_.stop();
                if (completed.symbol_address(
                        "_IOHIDEventCreateAccelerometerEvent"))
                    accelerometer_.start(state_->clock.now());
                state_->note_kernel_event_transition();
            });
        });
    registry_.register_function(
        "/IOKit", "_IOHIDEventSystemClose", [this](UserlandHleCall& call) {
            reset(call.process_id());
            call.resume_original_persistently();
        });
    registry_.register_function("/IOKit", "_IOHIDEventSystemClientCreate",
        [this](UserlandHleCall& call) {
            const auto process = call.process_id();
            call.resume_original_persistently(
                [this, process](UserlandHleCall& completed) {
                    const auto client = completed.argument(0);
                    if (client == 0U)
                        return;
                    EventSystemClient event_client;
                    event_client.process = process;
                    event_client.client = client;
                    event_clients_.insert_or_assign(
                        client, std::move(event_client));
                });
        });
    registry_.register_function("/IOKit",
        "_IOHIDEventSystemClientScheduleWithRunLoop",
        [this](UserlandHleCall& call) {
            const auto client = call.argument(0);
            if (const auto registered = event_clients_.find(client);
                registered != event_clients_.end()) {
                registered->second.processor = call.cpu().processor_id();
                refresh_event_client(client);
            }
            call.resume_original_persistently();
        });
    registry_.register_function("/IOKit",
        "_IOHIDEventSystemClientRegisterEventCallback",
        [this](UserlandHleCall& call) {
            const auto client = call.argument(0);
            if (const auto registered = event_clients_.find(client);
                registered != event_clients_.end()) {
                registered->second.callback = call.argument(1);
                registered->second.target = call.argument(2);
                registered->second.refcon = call.argument(3);
                refresh_event_client(client);
            }
            call.resume_original_persistently();
        });
    registry_.register_function("/IOKit",
        "_IOHIDEventSystemClientUnregisterEventCallback",
        [this](UserlandHleCall& call) {
            const auto client = call.argument(0);
            if (const auto registered = event_clients_.find(client);
                registered != event_clients_.end()) {
                registered->second.callback = 0U;
                registered->second.target = 0U;
                registered->second.refcon = 0U;
                refresh_event_client(client);
            }
            call.resume_original_persistently();
        });
    registry_.register_function("/IOKit",
        "_IOHIDEventSystemClientUnscheduleWithRunLoop",
        [this](UserlandHleCall& call) {
            const auto client = call.argument(0);
            if (const auto registered = event_clients_.find(client);
                registered != event_clients_.end()) {
                registered->second.processor.reset();
                refresh_event_client(client);
            }
            call.resume_original_persistently();
        });
    const auto set_matching = [this](UserlandHleCall& call) {
        const auto client = call.argument(0);
        if (const auto registered = event_clients_.find(client);
            registered != event_clients_.end()) {
            // The shared host observer stream currently represents the
            // digitizer service. A non-null matching object opts this client
            // into that stream; both single and multiple APIs share it.
            registered->second.digitizer_matching = call.argument(1) != 0U;
            refresh_event_client(client);
        }
        call.resume_original_persistently();
    };
    registry_.register_function(
        "/IOKit", "_IOHIDEventSystemClientSetMatching", set_matching);
    registry_.register_function(
        "/IOKit", "_IOHIDEventSystemClientSetMatchingMultiple", set_matching);
    for (const auto symbol :
        { "_IOHIDEventCreateDigitizerEvent", "_IOHIDEventCreateKeyboardEvent",
            "_IOHIDEventAppendEvent", "__IOHIDEventSystemDispatchEvent",
            "_IOHIDEventCreateAccelerometerEvent" }) {
        registry_.register_guest_function("/IOKit", symbol);
    }
    registry_.register_guest_function("/CoreFoundation", "_CFRelease");
}

void HidEventSystemHle::set_shared_state(
    std::shared_ptr<KernelSharedState> state)
{
    state_ = std::move(state);
}

void HidEventSystemHle::reset(std::uint32_t process)
{
    if (state_)
        state_->hid_event_queue.close(process);
    std::erase_if(event_clients_, [process](const auto& entry) {
        return entry.second.process == process;
    });
    consumer_process_ = 0U;
    delivering_processors_.clear();
    accelerometer_.stop();
    if (state_)
        state_->note_kernel_event_transition();
}

void HidEventSystemHle::refresh_event_client(std::uint32_t client)
{
    const auto registered = event_clients_.find(client);
    if (registered == event_clients_.end() || !state_)
        return;
    const auto& event_client = registered->second;
    if (!event_client.processor || event_client.callback == 0U ||
        !event_client.digitizer_matching) {
        state_->hid_event_queue.unobserve(
            event_client.process, event_client.client);
        state_->note_kernel_event_transition();
        return;
    }
    state_->hid_event_queue.observe(
        HidEventQueue::Observer { event_client.process, *event_client.processor,
            event_client.client, event_client.callback, event_client.target,
            event_client.refcon, true });
    state_->note_kernel_event_transition();
}

bool HidEventSystemHle::is_event_consumer(
    std::uint32_t process, std::size_t processor) const
{
    return state_ && state_->hid_event_queue.is_receiver(process, processor);
}

bool HidEventSystemHle::prepare_pending_event(
    Cpu& cpu, std::uint32_t process, std::uint32_t svc_immediate)
{
    const auto processor = cpu.processor_id();
    if (!is_event_consumer(process, processor) || !state_ ||
        delivering_processors_.contains(processor) || svc_immediate != 0x80U ||
        static_cast<std::int32_t>(cpu.registers()[12]) != -31 ||
        cpu.registers()[2] != 0U || (cpu.registers()[1] & 2U) == 0U)
        return false;
    if (auto observed =
            state_->hid_event_queue.take_observer(process, processor)) {
        delivering_processors_.insert(processor);
        const auto queued =
            HidEventTransaction::enqueue(registry_, observed->observer,
                std::move(observed->event), state_->user_interface_geometry,
                state_->darwin_abi.hid_digitizer, [this, processor] {
                    delivering_processors_.erase(processor);
                    if (state_)
                        state_->note_kernel_event_transition();
                });
        if (!queued)
            delivering_processors_.erase(processor);
        return queued;
    }
    if (consumer_process_ != process || consumer_processor_ != processor)
        return false;
    const auto consumer = state_->hid_event_queue.consumer();
    auto event = state_->hid_event_queue.take(process, cpu.processor_id());
    if (consumer && !event) {
        event = accelerometer_.sample(state_->clock.now());
        if (event)
            state_->note_kernel_event_transition();
    }
    if (!consumer || !event)
        return false;
    delivering_processors_.insert(processor);
    const auto queued = HidEventTransaction::enqueue(registry_, *consumer,
        *event, state_->user_interface_geometry,
        state_->darwin_abi.hid_digitizer, [this, processor] {
            delivering_processors_.erase(processor);
            if (state_)
                state_->note_kernel_event_transition();
        });
    if (!queued)
        delivering_processors_.erase(processor);
    return queued;
}

} // namespace ilemu
