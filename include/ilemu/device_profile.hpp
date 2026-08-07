#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ilemu/display_geometry.hpp"
#include "ilemu/arm_cpu_model.hpp"
#include "ilemu/guest_cpu_topology.hpp"

namespace ilemu {

// The simulator can expose the character-device ABI while declaring whether a
// physical baseband transport is actually present. This is a capability
// profile, not a firmware-version or application rule. The virtual mode is
// retained for explicit transport fixtures/replay; normal boots select the
// offline mode because no modem is attached to the host. Unavailable is
// reserved for a true no-device boot policy.
enum class BasebandTransportProfile : std::uint8_t {
    Virtual,
    // The device-node/IOKit ABI is present so stock CommCenter can complete
    // startup, but no radio notifications are authoritative. This is the
    // normal simulator profile and does not synthesize AT replies.
    Offline,
    Unavailable,
};

// Guest-visible graphics accelerator family. This describes the firmware
// capability boundary; it does not select the host renderer implementation.
enum class GraphicsAcceleratorProfileKind : std::uint8_t {
    MbxLite,
    Sgx535,
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
    GraphicsAcceleratorProfileKind graphics_accelerator{
        GraphicsAcceleratorProfileKind::MbxLite};
    // Bundle selected by the firmware's graphics service. Empty means the
    // accelerator exposes only the legacy MBX service and has no private
    // driver bundle to publish through IOAcceleratorES.
    std::string_view graphics_driver_bundle;
    BasebandTransportProfile baseband_transport{
        BasebandTransportProfile::Virtual};

    static const DeviceProfile& default_profile();
    [[nodiscard]] static std::span<const DeviceProfile> available_profiles();
    [[nodiscard]] static const DeviceProfile* find(std::string_view product_type);
};

}  // namespace ilemu
