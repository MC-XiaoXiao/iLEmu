// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "hid_service_delivery.hpp"

#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"

#include <memory>

namespace ilemu {
namespace {

    constexpr auto image = "/IOKit";
    constexpr auto callback_symbol = "_ilemu_HidServiceDelivery";

    class NativeServiceCallback
        : public std::enable_shared_from_this<NativeServiceCallback> {
    public:
        explicit NativeServiceCallback(std::uint32_t context)
            : context_ { context }
        {
        }

        void start(UserlandHleCall& call) { invoke(call); }

    private:
        enum class Step { Service, Event, Dispatch, Release };

        void invoke(UserlandHleCall& call)
        {
            auto& r = call.cpu().registers();
            const char* symbol = nullptr;
            switch (step_) {
            case Step::Service:
            case Step::Event:
                r[0] = context_;
                r[1] = step_ == Step::Service ? 0U : 1U;
                symbol = "_CFArrayGetValueAtIndex";
                break;
            case Step::Dispatch:
                r[0] = service_;
                r[1] = event_;
                symbol = "__IOHIDServiceDispatchEvent";
                break;
            case Step::Release:
                r[0] = context_;
                symbol = "_CFRelease";
                break;
            }
            static_cast<void>(call.call_guest_function(symbol,
                [self = shared_from_this()](UserlandHleCall& completed) {
                    self->complete(completed);
                }));
        }

        void complete(UserlandHleCall& call)
        {
            switch (step_) {
            case Step::Service:
                service_ = call.argument(0);
                step_ = Step::Event;
                break;
            case Step::Event:
                event_ = call.argument(0);
                step_ = Step::Dispatch;
                break;
            case Step::Dispatch:
                step_ = Step::Release;
                break;
            case Step::Release:
                call.set_return(0U);
                return;
            }
            invoke(call);
        }

        std::uint32_t context_;
        std::uint32_t service_ { };
        std::uint32_t event_ { };
        Step step_ { Step::Service };
    };

} // namespace

void HidServiceDelivery::register_functions(UserlandHleRegistry& registry)
{
    registry.register_function(
        image, callback_symbol, [](UserlandHleCall& call) {
            std::make_shared<NativeServiceCallback>(call.argument(0))
                ->start(call);
        });
    registry.register_guest_function(image, "__IOHIDServiceCopyDispatchQueue");
    registry.register_guest_function(image, "__IOHIDServiceDispatchEvent");
    registry.register_guest_function("/CoreFoundation", "_CFArrayCreate");
    registry.register_guest_function(
        "/CoreFoundation", "_CFArrayGetValueAtIndex");
    registry.register_guest_data_symbol(
        "/CoreFoundation", "_kCFTypeArrayCallBacks");
    registry.register_guest_function("/libdispatch.dylib", "_dispatch_async_f");
    registry.register_guest_function("/libdispatch.dylib", "_dispatch_release");
}

bool HidServiceDelivery::available(const UserlandHleCall& call)
{
    return call.symbol_address("__IOHIDServiceCopyDispatchQueue") &&
           call.symbol_address("__IOHIDServiceDispatchEvent") &&
           call.symbol_address("_CFArrayCreate") &&
           call.symbol_address("_CFArrayGetValueAtIndex") &&
           call.symbol_address("_kCFTypeArrayCallBacks") &&
           call.symbol_address("_dispatch_async_f") &&
           call.symbol_address("_dispatch_release");
}

std::optional<std::uint32_t> HidServiceDelivery::callback(UserlandHleCall& call)
{
    return call.callable_handler(image, callback_symbol, 0U);
}

} // namespace ilemu
