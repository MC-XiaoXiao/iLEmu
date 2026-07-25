#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ilegacysim/display_geometry.hpp"
#include "ilegacysim/arm_cpu_model.hpp"

namespace ilegacysim {

struct DeviceProfile {
    std::string_view product_type;
    std::string_view board_config;
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
    std::size_t physical_cpu_count;
    // Guest-visible panel/framebuffer size.
    DisplayGeometry display;
    // Native firmware layout and touch coordinate space. Older UIKit builds
    // may keep this fixed even when a different panel geometry is reported.
    DisplayGeometry user_interface;

    static const DeviceProfile& default_profile();
};

}  // namespace ilegacysim
