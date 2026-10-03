// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Construct and dispatch firmware-owned HID objects on their consumer thread.

#include "hid_event_transaction.hpp"

#include "device_state/darwin_abi.hpp"
#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"

#include <array>
#include <bit>
#include <memory>
#include <utility>

namespace ilemu {
namespace {

    // Darwin's ARM32 C ABI packs the uint64 timestamp into r1/r2 and
    // passes CGFloat coordinates as float words on the stack. The firmware
    // owns the event layout, collection metadata and reference counting.
    struct Arm32DigitizerEventProfile {
        static constexpr std::uint32_t finger = 2U;
        static constexpr std::uint32_t hand = 3U;
        static constexpr std::uint32_t range_changed = 1U;
        static constexpr std::uint32_t touch_changed = 2U;
        static constexpr std::uint32_t position_changed = 4U;
        static constexpr std::uint32_t identity_changed = 0x20U;
        static constexpr std::uint32_t cancelled = 0x80U;
        // Grape digitizer origin qualifiers consumed by native edge recognizers.
        static constexpr std::uint32_t edge_tip = 0x800U;
        static constexpr std::uint32_t corner = 0x1000U;
        static constexpr float edge_extent = 10.0F;
        // Contacts originate on the built-in display, not an indirect pad.
        static constexpr std::uint32_t display_integrated = 0x80000U;
        static constexpr std::uint32_t stack_scratch_bytes = 64U;
    };

    constexpr auto create_digitizer = "_IOHIDEventCreateDigitizerEvent";
    constexpr auto create_keyboard = "_IOHIDEventCreateKeyboardEvent";
    constexpr auto create_accelerometer = "_IOHIDEventCreateAccelerometerEvent";
    constexpr auto dispatch_service = "___IOHIDServiceEventCallback";
    constexpr auto copy_service = "_IOHIDEventSystemCopyService";
    constexpr auto create_number = "_CFNumberCreate";

    void prepare_digitizer(UserlandHleCall& call,
        const HidEventQueue::Event& event, float width, float height,
        bool collection, bool collection_contact_changes,
        DarwinHidDigitizerAbi digitizer_abi)
    {
        const auto& touch = std::get<TouchInput>(event.input);
        const auto active =
            touch.phase == TouchPhase::Down || touch.phase == TouchPhase::Move;
        using Profile = Arm32DigitizerEventProfile;
        auto mask = Profile::position_changed;
        // Raw IOHIDEventSystemClient callbacks consume collection lifecycle
        // changes. Darwin's primary dispatcher may instead require the
        // device-profiled child-only contract, so keep the roles independent.
        if (!collection || collection_contact_changes ||
            digitizer_abi != DarwinHidDigitizerAbi::ChildContactChanges) {
            if (touch.phase != TouchPhase::Move)
                mask |= Profile::range_changed | Profile::touch_changed;
            if (touch.phase == TouchPhase::Down)
                mask |= Profile::identity_changed;
            if (touch.phase == TouchPhase::Cancel)
                mask |= Profile::cancelled;
        }
        if (collection && event.contact_origin &&
            digitizer_abi ==
                DarwinHidDigitizerAbi::CollectionContactChangesWithEdgeOrigin) {
            const auto& origin = *event.contact_origin;
            const bool horizontal_edge =
                origin[0] < Profile::edge_extent ||
                origin[0] >= width - Profile::edge_extent;
            const bool vertical_edge =
                origin[1] < Profile::edge_extent ||
                origin[1] >= height - Profile::edge_extent;
            if (horizontal_edge && vertical_edge)
                mask |= Profile::corner;
            else if (horizontal_edge || vertical_edge)
                mask |= Profile::edge_tip;
        }
        auto& r = call.cpu().registers();
        r[13] -= Profile::stack_scratch_bytes;
        r[0] = 0U;
        r[1] = static_cast<std::uint32_t>(event.timestamp);
        r[2] = static_cast<std::uint32_t>(event.timestamp >> 32U);
        r[3] = collection ? Profile::hand : Profile::finger;
        const std::array<std::uint32_t, 12> arguments { 1U,
            collection ? 1U : event.identity, mask, 0U,
            std::bit_cast<std::uint32_t>(touch.x / width),
            std::bit_cast<std::uint32_t>(touch.y / height), 0U,
            std::bit_cast<std::uint32_t>(active ? 1.0F : 0.0F), 0U,
            active ? 1U : 0U, active ? 1U : 0U, Profile::display_integrated };
        static_cast<void>(call.memory().copy_in(
            r[13], std::as_bytes(std::span { arguments })));
    }

    class NativeEventTransaction
        : public std::enable_shared_from_this<NativeEventTransaction> {
    public:
        using Completion = std::function<void()>;
        NativeEventTransaction(HidEventQueue::Event event, std::uint32_t system,
            DisplayGeometry geometry, DarwinHidDigitizerAbi digitizer_abi,
            std::uint64_t sender_id, Completion completion)
            : event_ { event }
            , system_ { system }
            , geometry_ { geometry }
            , digitizer_abi_ { digitizer_abi }
            , sender_id_ { sender_id }
            , completion_ { std::move(completion) }
            , step_ { std::holds_alternative<TouchInput>(event.input)
                          ? Step::Hand
                      : std::holds_alternative<HidEventQueue::KeyboardInput>(
                            event.input)
                          ? Step::Keyboard
                          : Step::Accelerometer }
        {
        }
        NativeEventTransaction(HidEventQueue::Event event,
            HidEventQueue::Observer observer, DisplayGeometry geometry,
            DarwinHidDigitizerAbi digitizer_abi, std::uint64_t sender_id,
            Completion completion)
            : event_ { event }
            , observer_ { observer }
            , geometry_ { geometry }
            , digitizer_abi_ { digitizer_abi }
            , sender_id_ { sender_id }
            , completion_ { std::move(completion) }
            , step_ { Step::Hand }
        {
        }

        bool start(UserlandHleRegistry& registry, std::size_t processor)
        {
            return registry.queue_guest_function(
                step_ == Step::Hand       ? create_digitizer
                : step_ == Step::Keyboard ? create_keyboard
                                          : create_accelerometer,
                processor,
                [self = shared_from_this()](
                    UserlandHleCall& call) { self->setup(call); },
                [self = shared_from_this()](
                    UserlandHleCall& call) { self->complete(call); });
        }

    private:
        enum class Step {
            Accelerometer,
            Keyboard,
            Hand,
            Finger,
            Append,
            ReleaseFinger,
            SetSender,
            CreateSenderNumber,
            CopyService,
            ReleaseSenderNumber,
            DispatchService,
            ReleaseService,
            Dispatch,
            Callback,
            ReleaseEvent
        };
        void setup(UserlandHleCall& call)
        {
            auto& r = call.cpu().registers();
            switch (step_) {
            case Step::Accelerometer: {
                const auto& acceleration =
                    std::get<HidEventQueue::Acceleration>(event_.input);
                r[13] -= 16U;
                r[0] = 0U;
                r[1] = static_cast<std::uint32_t>(event_.timestamp);
                r[2] = static_cast<std::uint32_t>(event_.timestamp >> 32U);
                r[3] = std::bit_cast<std::uint32_t>(acceleration.x);
                const std::array<std::uint32_t, 3> arguments {
                    std::bit_cast<std::uint32_t>(acceleration.y),
                    std::bit_cast<std::uint32_t>(acceleration.z), 0U
                };
                static_cast<void>(call.memory().copy_in(
                    r[13], std::as_bytes(std::span { arguments })));
                break;
            }
            case Step::Keyboard: {
                const auto& key =
                    std::get<HidEventQueue::KeyboardInput>(event_.input);
                // Darwin ARM32 packs timestamp into r1/r2, with usage,
                // down and options as the three remaining stack arguments.
                r[13] -= 16U;
                r[0] = 0U;
                r[1] = static_cast<std::uint32_t>(event_.timestamp);
                r[2] = static_cast<std::uint32_t>(event_.timestamp >> 32U);
                r[3] = key.usage_page;
                const std::array<std::uint32_t, 3> arguments { key.usage,
                    key.down ? 1U : 0U, 0U };
                static_cast<void>(call.memory().copy_in(
                    r[13], std::as_bytes(std::span { arguments })));
                break;
            }
            case Step::Hand:
            case Step::Finger:
                prepare_digitizer(call, event_,
                    static_cast<float>(geometry_.width),
                    static_cast<float>(geometry_.height), step_ == Step::Hand,
                    observer_.has_value(), digitizer_abi_);
                break;
            case Step::Append:
                r[0] = root_event_;
                r[1] = finger_;
                r[2] = 0U;
                break;
            case Step::ReleaseFinger:
                r[0] = finger_;
                break;
            case Step::SetSender:
                // Darwin ARM32 packs the registry entry ID into r1/r2.
                r[0] = root_event_;
                r[1] = static_cast<std::uint32_t>(sender_id_);
                r[2] = static_cast<std::uint32_t>(sender_id_ >> 32U);
                break;
            case Step::CreateSenderNumber:
                r[13] -= 16U;
                r[0] = 0U;
                r[1] = 4U; // kCFNumberSInt64Type
                r[2] = r[13];
                static_cast<void>(call.memory().write32(r[13],
                    static_cast<std::uint32_t>(sender_id_)));
                static_cast<void>(call.memory().write32(r[13] + 4U,
                    static_cast<std::uint32_t>(sender_id_ >> 32U)));
                break;
            case Step::CopyService:
                r[0] = system_;
                r[1] = sender_number_;
                break;
            case Step::ReleaseSenderNumber:
                r[0] = sender_number_;
                break;
            case Step::DispatchService:
                // Enter the firmware's hardware service callback. It owns
                // device metadata, service filters and session routing.
                r[13] -= 16U;
                r[0] = service_;
                r[1] = 0U;
                r[2] = 0U;
                r[3] = root_event_;
                static_cast<void>(call.memory().write32(r[13], 0U));
                break;
            case Step::ReleaseService:
                r[0] = service_;
                break;
            case Step::Dispatch:
                r[0] = system_;
                r[1] = root_event_;
                break;
            case Step::Callback:
                r[0] = observer_->target;
                r[1] = observer_->refcon;
                r[2] = observer_->client;
                r[3] = root_event_;
                break;
            case Step::ReleaseEvent:
                r[0] = root_event_;
                break;
            }
        }

        void complete(UserlandHleCall& call)
        {
            const char* symbol = nullptr;
            switch (step_) {
            case Step::Accelerometer:
            case Step::Keyboard:
                root_event_ = call.argument(0);
                if (!root_event_) {
                    completion_();
                    return;
                }
                step_ = Step::Dispatch;
                symbol = "__IOHIDEventSystemDispatchEvent";
                break;
            case Step::Hand:
                root_event_ = call.argument(0);
                if (!root_event_) {
                    completion_();
                    return;
                }
                step_ = Step::Finger;
                symbol = create_digitizer;
                break;
            case Step::Finger:
                finger_ = call.argument(0);
                step_ = finger_ ? Step::Append : Step::ReleaseEvent;
                symbol = finger_ ? "_IOHIDEventAppendEvent" : "_CFRelease";
                break;
            case Step::Append:
                step_ = Step::ReleaseFinger;
                symbol = "_CFRelease";
                break;
            case Step::ReleaseFinger:
                if (sender_id_ && call.symbol_address("_IOHIDEventSetSenderID")) {
                    step_ = Step::SetSender;
                    symbol = "_IOHIDEventSetSenderID";
                    break;
                }
                [[fallthrough]];
            case Step::SetSender:
                if (observer_) {
                    step_ = Step::Callback;
                } else if (sender_id_ && call.symbol_address(dispatch_service) &&
                    call.symbol_address(copy_service) &&
                    call.symbol_address(create_number)) {
                    step_ = Step::CreateSenderNumber;
                    symbol = create_number;
                } else {
                    step_ = Step::Dispatch;
                    symbol = "__IOHIDEventSystemDispatchEvent";
                }
                break;
            case Step::CreateSenderNumber:
                sender_number_ = call.argument(0);
                step_ = sender_number_ ? Step::CopyService : Step::Dispatch;
                symbol = sender_number_ ? copy_service
                                        : "__IOHIDEventSystemDispatchEvent";
                break;
            case Step::CopyService:
                service_ = call.argument(0);
                step_ = Step::ReleaseSenderNumber;
                symbol = "_CFRelease";
                break;
            case Step::ReleaseSenderNumber:
                step_ = service_ ? Step::DispatchService : Step::Dispatch;
                symbol = service_ ? dispatch_service
                                  : "__IOHIDEventSystemDispatchEvent";
                break;
            case Step::DispatchService:
                step_ = Step::ReleaseService;
                symbol = "_CFRelease";
                break;
            case Step::ReleaseService:
            case Step::Dispatch:
            case Step::Callback:
                step_ = Step::ReleaseEvent;
                symbol = "_CFRelease";
                break;
            case Step::ReleaseEvent:
                completion_();
                return;
            }
            const auto setup = [self = shared_from_this()](
                                   UserlandHleCall& next) {
                self->setup(next);
            };
            const auto completed = [self = shared_from_this()](
                                       UserlandHleCall& next) {
                self->complete(next);
            };
            const auto queued = step_ == Step::Callback
                                    ? call.continue_deferred_guest_callback(
                                          observer_->callback, setup, completed)
                                    : call.continue_deferred_guest_function(
                                          symbol, setup, completed);
            if (!queued) {
                completion_();
            }
        }
        HidEventQueue::Event event_;
        std::uint32_t system_ { };
        std::optional<HidEventQueue::Observer> observer_;
        DisplayGeometry geometry_;
        DarwinHidDigitizerAbi digitizer_abi_;
        std::uint64_t sender_id_ { };
        Completion completion_;
        Step step_;
        std::uint32_t root_event_ { };
        std::uint32_t finger_ { };
        std::uint32_t sender_number_ { };
        std::uint32_t service_ { };
    };

} // namespace

bool HidEventTransaction::enqueue(UserlandHleRegistry& registry,
    const HidEventQueue::Consumer& consumer, HidEventQueue::Event event,
    DisplayGeometry geometry, DarwinHidDigitizerAbi digitizer_abi,
    std::uint64_t sender_id, std::function<void()> completion)
{
    return std::make_shared<NativeEventTransaction>(std::move(event),
        consumer.system, geometry, digitizer_abi, sender_id, std::move(completion))
        ->start(registry, consumer.processor);
}

bool HidEventTransaction::enqueue(UserlandHleRegistry& registry,
    const HidEventQueue::Observer& observer, HidEventQueue::Event event,
    DisplayGeometry geometry, DarwinHidDigitizerAbi digitizer_abi,
    std::uint64_t sender_id, std::function<void()> completion)
{
    return std::make_shared<NativeEventTransaction>(std::move(event), observer,
        geometry, digitizer_abi, sender_id, std::move(completion))
        ->start(registry, observer.processor);
}

} // namespace ilemu
