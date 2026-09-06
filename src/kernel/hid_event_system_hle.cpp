#include "kernel/hid_event_system_hle.hpp"

#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"
#include "kernel/kernel_shared_state.hpp"

#include <array>
#include <bit>

namespace ilemu {
namespace {

    // Darwin's ARM32 C ABI packs the uint64 timestamp into r1/r2 and
    // passes CGFloat coordinates as float words on the stack. The firmware
    // owns the event layout, collection metadata and reference counting.
    struct Arm32DigitizerEventProfile {
        static constexpr std::uint32_t finger = 0x22U;
        static constexpr std::uint32_t hand = 0x23U;
        static constexpr std::uint32_t range_changed = 1U;
        static constexpr std::uint32_t touch_changed = 2U;
        static constexpr std::uint32_t position_changed = 4U;
        static constexpr std::uint32_t identity_changed = 0x20U;
        static constexpr std::uint32_t cancelled = 0x80U;
        static constexpr std::uint32_t stack_scratch_bytes = 64U;
    };

    constexpr auto create_digitizer = "_IOHIDEventCreateDigitizerEvent";

    void prepare_digitizer(UserlandHleCall& call,
        const HidEventQueue::Event& event, float width, float height,
        bool collection)
    {
        const auto active = event.touch.phase == TouchPhase::Down ||
                            event.touch.phase == TouchPhase::Move;
        using Profile = Arm32DigitizerEventProfile;
        auto mask = Profile::position_changed;
        if (event.touch.phase != TouchPhase::Move)
            mask |= Profile::range_changed | Profile::touch_changed;
        if (event.touch.phase == TouchPhase::Down)
            mask |= Profile::identity_changed;
        if (event.touch.phase == TouchPhase::Cancel)
            mask |= Profile::cancelled;
        auto& r = call.cpu().registers();
        r[13] -= Profile::stack_scratch_bytes;
        r[0] = 0U;
        r[1] = static_cast<std::uint32_t>(event.timestamp);
        r[2] = static_cast<std::uint32_t>(event.timestamp >> 32U);
        r[3] = collection ? Profile::hand : Profile::finger;
        const std::array<std::uint32_t, 12> arguments { 1U,
            collection ? 1U : event.identity, mask, 0U,
            std::bit_cast<std::uint32_t>(event.touch.x / width),
            std::bit_cast<std::uint32_t>(event.touch.y / height), 0U,
            std::bit_cast<std::uint32_t>(active ? 1.0F : 0.0F), 0U,
            active ? 1U : 0U, active ? 1U : 0U, 0U };
        static_cast<void>(call.memory().copy_in(
            r[13], std::as_bytes(std::span { arguments })));
    }

    class DigitizerTransaction
        : public std::enable_shared_from_this<DigitizerTransaction> {
    public:
        using Completion = std::function<void()>;
        DigitizerTransaction(HidEventQueue::Event event, std::uint32_t system,
            DisplayGeometry geometry, Completion completion)
            : event_ { event }
            , system_ { system }
            , geometry_ { geometry }
            , completion_ { std::move(completion) }
        {
        }

        bool start(UserlandHleRegistry& registry, std::size_t processor)
        {
            return registry.queue_guest_function(
                create_digitizer, processor,
                [self = shared_from_this()](
                    UserlandHleCall& call) { self->setup(call); },
                [self = shared_from_this()](
                    UserlandHleCall& call) { self->complete(call); });
        }

    private:
        enum class Step {
            Hand,
            Finger,
            Append,
            ReleaseFinger,
            Dispatch,
            ReleaseHand
        };
        void setup(UserlandHleCall& call)
        {
            auto& r = call.cpu().registers();
            switch (step_) {
            case Step::Hand:
            case Step::Finger:
                prepare_digitizer(call, event_,
                    static_cast<float>(geometry_.width),
                    static_cast<float>(geometry_.height), step_ == Step::Hand);
                break;
            case Step::Append:
                r[0] = hand_;
                r[1] = finger_;
                r[2] = 0U;
                break;
            case Step::ReleaseFinger:
                r[0] = finger_;
                break;
            case Step::Dispatch:
                r[0] = system_;
                r[1] = hand_;
                break;
            case Step::ReleaseHand:
                r[0] = hand_;
                break;
            }
        }

        void complete(UserlandHleCall& call)
        {
            const char* symbol = nullptr;
            switch (step_) {
            case Step::Hand:
                hand_ = call.argument(0);
                if (!hand_) {
                    completion_();
                    return;
                }
                step_ = Step::Finger;
                symbol = create_digitizer;
                break;
            case Step::Finger:
                finger_ = call.argument(0);
                step_ = finger_ ? Step::Append : Step::ReleaseHand;
                symbol = finger_ ? "_IOHIDEventAppendEvent" : "_CFRelease";
                break;
            case Step::Append:
                step_ = Step::ReleaseFinger;
                symbol = "_CFRelease";
                break;
            case Step::ReleaseFinger:
                step_ = Step::Dispatch;
                symbol = "__IOHIDEventSystemDispatchEvent";
                break;
            case Step::Dispatch:
                step_ = Step::ReleaseHand;
                symbol = "_CFRelease";
                break;
            case Step::ReleaseHand:
                completion_();
                return;
            }
            if (!call.continue_deferred_guest_function(
                    symbol,
                    [self = shared_from_this()](
                        UserlandHleCall& next) { self->setup(next); },
                    [self = shared_from_this()](
                        UserlandHleCall& next) { self->complete(next); })) {
                completion_();
            }
        }
        HidEventQueue::Event event_;
        std::uint32_t system_;
        DisplayGeometry geometry_;
        Completion completion_;
        Step step_ { Step::Hand };
        std::uint32_t hand_ { };
        std::uint32_t finger_ { };
    };

} // namespace

HidEventSystemHle::HidEventSystemHle(UserlandHleRegistry& registry)
    : registry_ { registry }
{
    registry_.register_function(
        "/IOKit", "_IOHIDEventSystemOpen", [this](UserlandHleCall& call) {
            const HidEventQueue::Consumer consumer { call.process_id(),
                call.cpu().processor_id(), call.argument(0) };
            const auto callback = call.argument(1);
            call.resume_original_persistently(
                [this, consumer, callback](UserlandHleCall& completed) {
                    if (!state_ || !callback || completed.argument(0) == 0U ||
                        !state_->user_interface_geometry.valid())
                        return;
                    for (const auto symbol :
                        { create_digitizer, "_IOHIDEventAppendEvent",
                            "__IOHIDEventSystemDispatchEvent", "_CFRelease" }) {
                        if (!completed.symbol_address(symbol))
                            return;
                    }
                    consumer_process_ = consumer.process;
                    consumer_processor_ = consumer.processor;
                    state_->hid_event_queue.open(consumer);
                });
        });
    registry_.register_function(
        "/IOKit", "_IOHIDEventSystemClose", [this](UserlandHleCall& call) {
            reset(call.process_id());
            call.resume_original_persistently();
        });
    for (const auto symbol : { create_digitizer, "_IOHIDEventAppendEvent",
             "__IOHIDEventSystemDispatchEvent" }) {
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
    consumer_process_ = 0U;
    delivering_ = false;
}

void HidEventSystemHle::prepare_pending_event(
    Cpu& cpu, std::uint32_t process, std::uint32_t svc_immediate)
{
    if (consumer_process_ != process ||
        consumer_processor_ != cpu.processor_id() || !state_ || delivering_ ||
        svc_immediate != 0x80U ||
        static_cast<std::int32_t>(cpu.registers()[12]) != -31 ||
        cpu.registers()[2] != 0U || (cpu.registers()[1] & 2U) == 0U)
        return;
    const auto consumer = state_->hid_event_queue.consumer();
    const auto event =
        state_->hid_event_queue.take(process, cpu.processor_id());
    if (!consumer || !event)
        return;
    auto transaction =
        std::make_shared<DigitizerTransaction>(*event, consumer->system,
            state_->user_interface_geometry, [this] { delivering_ = false; });
    delivering_ = transaction->start(registry_, cpu.processor_id());
}

} // namespace ilemu
