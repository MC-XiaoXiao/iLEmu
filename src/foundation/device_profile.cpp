#include "ilemu/device_profile.hpp"

namespace ilemu {

const DeviceProfile& DeviceProfile::default_profile() {
    // iPhone OS 1.0 used the pre-1.1.2 clock setting generally reported as
    // 400 MHz. The S5L8900/ARM1176 core is physically single-core; the emulator
    // may expose extra virtual execution cores for stress and scheduler testing.
    static constexpr DeviceProfile profile{
        "iPhone1,1",
        "M68AP",
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
    };
    return profile;
}

}  // namespace ilemu
