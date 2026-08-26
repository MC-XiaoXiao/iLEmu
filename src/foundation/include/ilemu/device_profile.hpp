#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ilemu/arm_cpu_model.hpp"
#include "ilemu/display_geometry.hpp"
#include "ilemu/guest_cpu_topology.hpp"

namespace ilemu {

// The simulator can expose a baseband transport for an explicit replay fixture
// or run with the normal offline/no-modem policy. This is a capability
// profile, not a firmware-version or application rule.
enum class BasebandTransportProfile : std::uint8_t {
    Virtual,
    // No physical modem is attached. Registry and control-plane probes stay
    // visible so stock clients settle on the normal Offline state, while no
    // guest input is injected and no host-bound modem output is produced.
    Offline,
};

// Guest-visible graphics accelerator family. This describes the firmware
// capability boundary; it does not select the host renderer implementation.
enum class GraphicsAcceleratorProfileKind : std::uint8_t {
    MbxLite,
    Sgx535,
};

// Host-driven system gestures are expressed in the firmware's normalized UI
// coordinate space. Keeping this data in the device profile avoids teaching
// the control frontend about product names, builds, or SpringBoard pages.
struct NormalizedDragGestureProfile {
    float start_x_fraction { };
    float start_y_fraction { };
    float end_x_fraction { };
    float end_y_fraction { };
    std::uint32_t duration_ms { };
    std::size_t steps { };
    std::uint32_t release_delay_ms { };
    std::uint32_t wake_settle_delay_ms { };
};

struct SystemGestureProfile {
    std::string_view name;
    NormalizedDragGestureProfile unlock;
    // Some compact lock scenes need the firmware Home transition even while
    // the panel is already powered; tablet scenes retain the power-only wake
    // behavior.
    bool home_wake_barrier { };
};

inline constexpr SystemGestureProfile classic_compact_system_gestures {
    "classic-compact-slider",
    { 0.15625F, 0.8958333333F, 0.8125F, 0.8958333333F, 1'400U, 7U, 200U,
        1'000U },
    true,
};

inline constexpr SystemGestureProfile classic_centered_tablet_system_gestures {
    "classic-centered-tablet-slider",
    { 0.3776041667F, 0.9375F, 0.8463541667F, 0.9375F, 1'400U, 7U, 200U,
        1'500U },
    false,
};

// GraphicsServices publishes this dictionary through the GSCapabilities
// shared-memory object. The base dictionary is firmware-owned; these identity
// values and the multitasking switch describe the emulated device boundary
// used when that object has not been published yet.
struct GraphicsServicesCapabilityProfile {
    std::string_view device_name;
    std::string_view marketing_name;
    bool supports_multitasking { };
};

inline constexpr std::array<std::byte, 52>
    default_virtual_effaceable_storage_blob {
        std::byte { 0x01 }, std::byte { 0x00 }, std::byte { 0x00 },
        std::byte { 0x00 },
        std::byte { 0x00 }, std::byte { 0x01 }, std::byte { 0x02 },
        std::byte { 0x03 }, std::byte { 0x04 }, std::byte { 0x05 },
        std::byte { 0x06 }, std::byte { 0x07 }, std::byte { 0x08 },
        std::byte { 0x09 }, std::byte { 0x0a }, std::byte { 0x0b },
        std::byte { 0x0c }, std::byte { 0x0d }, std::byte { 0x0e },
        std::byte { 0x0f },
        std::byte { 0x10 }, std::byte { 0x11 }, std::byte { 0x12 },
        std::byte { 0x13 }, std::byte { 0x14 }, std::byte { 0x15 },
        std::byte { 0x16 }, std::byte { 0x17 }, std::byte { 0x18 },
        std::byte { 0x19 }, std::byte { 0x1a }, std::byte { 0x1b },
        std::byte { 0x1c }, std::byte { 0x1d }, std::byte { 0x1e },
        std::byte { 0x1f }, std::byte { 0x20 }, std::byte { 0x21 },
        std::byte { 0x22 }, std::byte { 0x23 }, std::byte { 0x24 },
        std::byte { 0x25 }, std::byte { 0x26 }, std::byte { 0x27 },
        std::byte { 0x28 }, std::byte { 0x29 }, std::byte { 0x2a },
        std::byte { 0x2b }, std::byte { 0x2c }, std::byte { 0x2d },
        std::byte { 0x2e }, std::byte { 0x2f },
    };

// The early ARMv7 firmware starts keybagd through the AppleKeyStore IOKit
// service. This is a device capability boundary: legacy profiles without the
// hardware-backed key store keep the service absent, while the virtualized
// ARMv7 profiles publish the matching guest-visible endpoint.
struct KeyBagCapabilityProfile {
    bool apple_key_store_available { };
    // A virtual data volume has no physical effaceable-storage locker. The
    // firmware's no-effaceable-storage property selects its ordinary plist
    // keybag path, which is the recoverable path used by this simulator.
    bool effaceable_storage_available { true };
    // iOS 4-era keybagd uses AppleEffaceableStorage directly. A virtual
    // endpoint keeps that firmware path intact when the guest has no physical
    // locker; its state is owned by the data-volume bootstrap profile.
    bool virtual_effaceable_storage_available { };
    std::array<std::byte, 52> virtual_effaceable_storage_blob {
        default_virtual_effaceable_storage_blob
    };
};

struct DeviceProfile {
    std::string_view product_type;
    std::string_view board_config;
    // CTL_HW/HW_MODEL identity. Keep this separate from board_config: the
    // latter is the physical IORegistry compatibility string, while an
    // activated simulator may use a development-board model so stock
    // Lockdown can take its firmware-provided no-baseband path.
    std::string_view hardware_model;
    // Model used by the explicit activated simulator profile. This is a
    // capability of the emulated device, not a firmware-version switch.
    std::string_view activation_hardware_model;
    // Retail configuration identifier exposed by the platform device tree.
    // This is distinct from product_type (for example, iPhone1,1) and the
    // hardware board configuration (for example, M68AP).
    std::string_view model_number;
    std::string_view soc;
    std::string_view cpu_core;
    std::string_view instruction_set;
    ArmCpuModelKind cpu_model;
    std::uint32_t cpu_hz;
    std::uint32_t bus_hz;
    std::uint64_t ram_bytes;
    // Nominal flash capacity for volume geometry. The guest sees this device
    // property, never the host filesystem's capacity.
    std::uint64_t storage_bytes;
    // Guest-visible topology. Host execution resources are intentionally not
    // stored here: adding host threads must never change the device contract.
    GuestCpuTopology guest_cpu_topology;
    // Guest-visible panel/framebuffer size.
    DisplayGeometry display;
    // Native firmware layout and touch coordinate space. Older UIKit builds
    // may keep this fixed even when a different panel geometry is reported.
    DisplayGeometry user_interface;
    SystemGestureProfile system_gestures { classic_compact_system_gestures };
    GraphicsAcceleratorProfileKind graphics_accelerator {
        GraphicsAcceleratorProfileKind::MbxLite
    };
    // Bundle selected by the firmware's graphics service. Empty means the
    // accelerator exposes only the legacy MBX service and has no private
    // driver bundle to publish through IOAcceleratorES.
    std::string_view graphics_driver_bundle;
    // IORegistry class of the physical LCD/framebuffer service. QuartzCore
    // discovers its native CAWindowServerDisplay through this device class;
    // host presentation remains a separate backend concern.
    std::string_view framebuffer_service_class;
    GraphicsServicesCapabilityProfile graphics_services_capabilities;
    KeyBagCapabilityProfile keybag_capabilities;
    // Default transport for a normal boot without --baseband-input. An
    // explicit replay input overrides this with Virtual.
    BasebandTransportProfile baseband_transport {
        BasebandTransportProfile::Virtual
    };
    // Whether this device profile has a guest-visible fixed baseband control
    // endpoint. This is a platform capability used by the transport boundary;
    // it is not a firmware-version or process-name rule. An explicit replay
    // transport always makes the endpoint available at boot.
    bool baseband_device_available { true };

    static const DeviceProfile& default_profile();
    [[nodiscard]] static std::span<const DeviceProfile> available_profiles();
    [[nodiscard]] static const DeviceProfile* find(
        std::string_view product_type);
};

} // namespace ilemu
