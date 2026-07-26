#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/system_button_input.hpp"
#include "ilemu/touch_input.hpp"

namespace ilemu {
class SceneCoordinator;
class UserlandHleRegistry;
}

namespace ilemu::graphics_services_input {

inline constexpr std::string_view system_event_service{"PurpleSystemEventPort"};
inline constexpr std::uint32_t application_will_resign_active_event_type =
    2002;

enum class EnqueueResult {
  Queued,
  Deferred,
};

struct ServiceResolution {
  std::uint32_t object{};
  std::size_t flushed_events{};
  bool application_event_port{};
  std::string service_name;
};

// iPhone OS 1.0 SpringBoard keeps alert items in its own top-level window
// stack without changing the foreground application lifecycle. Observe the
// stripped alert-item callbacks so host input follows that system-owned layer.
void register_springboard_alert_observers(
    UserlandHleRegistry &registry,
    std::function<void(std::uint32_t, bool)> observer);

// Extracts the leading GSEventRecord type from a message with id 123. This is
// shared by input injection and Mach tracing so application lifecycle events
// can be diagnosed without duplicating the private wire offsets.
[[nodiscard]] std::optional<std::uint32_t>
event_type(std::span<const std::byte> message);

// These two functions observe launchd's ordinary bootstrap MIG traffic. The
// caller must hold KernelSharedState::mach_mutex.
void record_bootstrap_lookup_locked(KernelSharedState &state,
                                    std::uint32_t reply_object,
                                    std::string_view service_name);
void record_bootstrap_registration_locked(KernelSharedState &state,
                                          std::string_view service_name);
[[nodiscard]] ServiceResolution record_bootstrap_reply_locked(
    KernelSharedState &state, std::uint32_t reply_object,
    std::span<const KernelSharedState::MachMessage::PortTransfer> transfers,
    std::uint32_t receiver_process_id = 0);

// Thread-safe host entry point. Input arriving before SpringBoard has resolved
// its event service is retained and flushed as soon as launchd replies.
[[nodiscard]] EnqueueResult enqueue_touch(KernelSharedState &state,
                                          const TouchInput &input,
                                          const SceneCoordinator *scenes =
                                              nullptr);

// A complete Home Down/Up pair wakes a sleeping lock screen before touch.
[[nodiscard]] EnqueueResult
enqueue_system_button(KernelSharedState &state, const SystemButtonInput &input);

// Publishes the firmware event for the new physical ringer/silent switch
// position. The switch value remains in the device-state provider.
[[nodiscard]] EnqueueResult
enqueue_ringer_switch_change(KernelSharedState &state, bool active);

// A resolved application port becomes foreground only when LayerKit attaches
// that application's client context; background application services must not
// steal SpringBoard touches.
void activate_resolved_application(KernelSharedState &state,
                                   std::uint32_t process_id,
                                   SceneCoordinator *scenes = nullptr);

// Starts a new generation for a server-side LayerKit render context. The next
// visible root commit binds the context to the then-pending App event route.
void reset_application_scene_context(KernelSharedState &state,
                                     std::uint32_t render_process_id,
                                     std::uint32_t context);

// Binds a version-adapter render context to the App process identified through
// its emulated Mach event route and retains legacy exit-snapshot geometry.
// Returns that App PID so the adapter can publish its native geometry to the
// common SceneCoordinator without teaching Mach IPC about LayerKit.
std::optional<std::uint32_t> record_application_scene_transform(
    KernelSharedState &state, std::uint32_t render_process_id,
    std::uint32_t context,
    const KernelSharedState::ApplicationTouchTransform &transform);

// Releases every cached scene/routing record owned by one exiting process.
// The caller holds mach_mutex because receive-right termination and scene
// invalidation must be one transaction. Explicit scene ownership remains
// authoritative even after the process has relinquished its receive right.
void release_application_process_locked(KernelSharedState &state,
                                        std::uint32_t process_id);

// HOME/lock temporarily return touch ownership to SpringBoard. Lock keeps the
// visible App scene transform available across the unlock lifecycle, while a
// real Home transition releases it after the exit snapshot is prepared.
void suspend_active_application(
    KernelSharedState &state,
    KernelSharedState::ApplicationSuspensionReason reason,
    SceneCoordinator *scenes = nullptr);

// Observes ordinary SpringBoard-to-application GSEvents while the caller holds
// KernelSharedState::mach_mutex. Foreground lifecycle events resume the saved
// application route after unlock; resign-active events suspend it.
void record_application_lifecycle_event_locked(KernelSharedState &state,
                                               std::uint32_t sender_pid,
                                               std::uint32_t destination,
                                               std::uint32_t event_type,
                                               SceneCoordinator *scenes =
                                                   nullptr);

} // namespace ilemu::graphics_services_input
