#include "ilemu/device_profile.hpp"

#include <array>

namespace ilemu {

namespace {

constexpr std::array<DeviceProfile, 3> profiles{
    DeviceProfile{
        "iPhone1,1",
        "M68AP",
        "M68AP",
        "M68DEV",
        "MA712LL",
        "Samsung S5L8900 (APL0098)",
        "ARM1176JZF-S",
        "ARMv6KZ + Thumb",
        ArmCpuModelKind::Arm1176JzfS,
        400'000'000,
        100'000'000,
        128ULL * 1024ULL * 1024ULL,
        8ULL * 1024ULL * 1024ULL * 1024ULL,
        1,
        default_display_geometry,
        default_display_geometry,
        GraphicsAcceleratorProfileKind::MbxLite,
        "",
    },
    DeviceProfile{
        "iPhone1,2",
        "N82AP",
        "N82AP",
        "N82DEV",
        "MB046",
        "Samsung S5L8900 (APL0098)",
        "ARM1176JZF-S",
        "ARMv6KZ + Thumb",
        ArmCpuModelKind::Arm1176JzfS,
        412'000'000,
        100'000'000,
        128ULL * 1024ULL * 1024ULL,
        8ULL * 1024ULL * 1024ULL * 1024ULL,
        1,
        default_display_geometry,
        default_display_geometry,
        GraphicsAcceleratorProfileKind::MbxLite,
        "",
    },
    DeviceProfile{
        "iPhone2,1",
        "N88AP",
        "N88AP",
        "N88DEV",
        "MB715",
        "Samsung S5L8920",
        "Cortex-A8",
        "ARMv7 + Thumb-2",
        ArmCpuModelKind::CortexA8,
        600'000'000,
        100'000'000,
        256ULL * 1024ULL * 1024ULL,
        16ULL * 1024ULL * 1024ULL * 1024ULL,
        1,
        default_display_geometry,
        default_display_geometry,
        GraphicsAcceleratorProfileKind::Sgx535,
        "IMGSGX535GLDriver",
    },
};

} // namespace

const DeviceProfile& DeviceProfile::default_profile() {
    return profiles.front();
}

std::span<const DeviceProfile> DeviceProfile::available_profiles() {
    return profiles;
}

const DeviceProfile* DeviceProfile::find(std::string_view product_type) {
    for (const auto& profile : profiles) {
        if (profile.product_type == product_type) {
            return &profile;
        }
    }
    return nullptr;
}

}  // namespace ilemu
