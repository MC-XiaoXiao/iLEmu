// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Describe virtual hardware identities, capabilities and device
// configuration profiles.

#include "foundation/device_model.hpp"

#include <array>

namespace ilemu {

namespace {

    constexpr std::array<DeviceModel, 8> models {
        DeviceModel {
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
            GuestCpuTopology::single_core(400'000'000U,
                GuestCpuPerformanceClass::Legacy,
                guest_cpu_isa::armv6k | guest_cpu_isa::thumb, 1U),
            default_display_geometry,
            default_display_geometry,
            classic_compact_system_gestures,
            GraphicsAcceleratorKind::MbxLite,
            "",
            "AppleH1CLCD",
            { "iPhone", "iPhone", false, true },
            { false },
            BasebandTransport::Offline,
            true,
            ActivationHardwareModelPolicy::DevelopmentBoard,
        },
        DeviceModel {
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
            GuestCpuTopology::single_core(412'000'000U,
                GuestCpuPerformanceClass::Legacy,
                guest_cpu_isa::armv6k | guest_cpu_isa::thumb, 2U),
            default_display_geometry,
            default_display_geometry,
            classic_compact_system_gestures,
            GraphicsAcceleratorKind::MbxLite,
            "",
            "AppleH1CLCD",
            { "iPhone", "iPhone 3G", false, true },
            // iPhone OS 4 still routes its no-passcode bootstrap through the
            // AppleKeyStore contract.  The service is virtualized here; the
            // model describes the guest capability rather than physical
            // silicon.
            { true, false, true },
            BasebandTransport::Offline,
            true,
        },
        DeviceModel {
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
            GuestCpuTopology::single_core(600'000'000U,
                GuestCpuPerformanceClass::Performance,
                guest_cpu_isa::armv7 | guest_cpu_isa::thumb |
                    guest_cpu_isa::thumb2,
                3U),
            default_display_geometry,
            default_display_geometry,
            classic_compact_system_gestures,
            GraphicsAcceleratorKind::Sgx535,
            "IMGSGX535GLDriver",
            "AppleM2CLCD",
            { "iPhone", "iPhone 3GS", true, true },
            { true, false, true },
            BasebandTransport::Offline,
            false,
        },
        DeviceModel {
            "iPhone3,1",
            "N90AP",
            "N90AP",
            "N90DEV",
            "MC603",
            "Apple A4 (S5L8930)",
            "Cortex-A8",
            "ARMv7 + Thumb-2",
            ArmCpuModelKind::CortexA8,
            1'000'000'000,
            100'000'000,
            512ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL * 1024ULL,
            GuestCpuTopology::single_core(1'000'000'000U,
                GuestCpuPerformanceClass::Performance,
                guest_cpu_isa::armv7 | guest_cpu_isa::thumb |
                    guest_cpu_isa::thumb2,
                7U),
            DisplayGeometry { 640U, 960U },
            DisplayGeometry { 320U, 480U },
            classic_compact_system_gestures,
            GraphicsAcceleratorKind::Sgx535,
            "IMGSGX535GLDriver",
            "AppleCLCD",
            { "iPhone", "iPhone 4", true, true },
            { true, false, true },
            BasebandTransport::Offline,
            true,
        },
        DeviceModel {
            "iPhone4,1",
            "N94AP",
            "N94AP",
            "N94DEV",
            "MD235",
            "Apple A5 (S5L8940)",
            "Cortex-A9",
            "ARMv7 + Thumb-2",
            ArmCpuModelKind::CortexA9,
            800'000'000,
            100'000'000,
            512ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL * 1024ULL,
            GuestCpuTopology::symmetric_cores(2U, 800'000'000U,
                GuestCpuPerformanceClass::Performance,
                guest_cpu_isa::armv7 | guest_cpu_isa::thumb |
                    guest_cpu_isa::thumb2,
                8U),
            DisplayGeometry { 640U, 960U },
            DisplayGeometry { 320U, 480U },
            classic_compact_system_gestures,
            GraphicsAcceleratorKind::Sgx543,
            "IMGSGX543GLDriver",
            "AppleM2CLCD",
            { "iPhone", "iPhone 4S", true, true },
            { true, false, true },
            BasebandTransport::Offline,
            true,
            ActivationHardwareModelPolicy::Retail,
            0,
            AudioHardwareProfile::CodecBasebandVoiceRouting,
            { "AppleM2TVOut", { 720U, 480U } },
        },
        DeviceModel {
            "iPod1,1",
            "N45AP",
            "N45AP",
            "N45DEV",
            "MA623",
            "Samsung S5L8900 (APL0098)",
            "ARM1176JZF-S",
            "ARMv6KZ + Thumb",
            ArmCpuModelKind::Arm1176JzfS,
            412'000'000,
            100'000'000,
            128ULL * 1024ULL * 1024ULL,
            8ULL * 1024ULL * 1024ULL * 1024ULL,
            GuestCpuTopology::single_core(412'000'000U,
                GuestCpuPerformanceClass::Legacy,
                guest_cpu_isa::armv6k | guest_cpu_isa::thumb, 5U),
            default_display_geometry,
            default_display_geometry,
            classic_compact_system_gestures,
            GraphicsAcceleratorKind::MbxLite,
            "",
            "AppleH1CLCD",
            { "iPod", "iPod touch", false, false },
            { false },
            BasebandTransport::Offline,
            false,
        },
        DeviceModel {
            "iPod2,1",
            "N72AP",
            "N72AP",
            "N72DEV",
            "MB528",
            "Samsung S5L8720",
            "ARM1176JZF-S",
            "ARMv6KZ + Thumb",
            ArmCpuModelKind::Arm1176JzfS,
            533'000'000,
            100'000'000,
            128ULL * 1024ULL * 1024ULL,
            8ULL * 1024ULL * 1024ULL * 1024ULL,
            GuestCpuTopology::single_core(533'000'000U,
                GuestCpuPerformanceClass::Legacy,
                guest_cpu_isa::armv6k | guest_cpu_isa::thumb, 6U),
            default_display_geometry,
            default_display_geometry,
            classic_compact_system_gestures,
            GraphicsAcceleratorKind::MbxLite,
            "",
            "AppleH1CLCD",
            { "iPod", "iPod touch", false, false },
            { false },
            BasebandTransport::Offline,
            false,
        },
        DeviceModel {
            "iPad1,1",
            "K48AP",
            "K48AP",
            "K48DEV",
            "MB292LL",
            "Apple A4 (S5L8930)",
            "Cortex-A8",
            "ARMv7 + Thumb-2",
            ArmCpuModelKind::CortexA8,
            1'000'000'000,
            100'000'000,
            256ULL * 1024ULL * 1024ULL,
            16ULL * 1024ULL * 1024ULL * 1024ULL,
            GuestCpuTopology::single_core(1'000'000'000U,
                GuestCpuPerformanceClass::Performance,
                guest_cpu_isa::armv7 | guest_cpu_isa::thumb |
                    guest_cpu_isa::thumb2,
                4U),
            DisplayGeometry { 768U, 1024U },
            DisplayGeometry { 768U, 1024U },
            classic_centered_tablet_system_gestures,
            GraphicsAcceleratorKind::Sgx535,
            "IMGSGX535GLDriver",
            "AppleM2CLCD",
            { "iPad", "iPad", true, false },
            { true, false, true },
            BasebandTransport::Offline,
            false,
            ActivationHardwareModelPolicy::Retail,
            247ULL * 1024ULL * 1024ULL,
        },
    };

} // namespace

const DeviceModel& DeviceModel::default_model()
{
    return models.front();
}

std::span<const DeviceModel> DeviceModel::available_models()
{
    return models;
}

const DeviceModel* DeviceModel::find(std::string_view product_type)
{
    for (const auto& model : models) {
        if (model.product_type == product_type) {
            return &model;
        }
    }
    return nullptr;
}

} // namespace ilemu
