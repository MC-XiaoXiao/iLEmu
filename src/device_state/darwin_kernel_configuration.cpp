#include "device_state/darwin_kernel_configuration.hpp"

#include "darwin_firmware_identity.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace ilemu {
namespace {

    auto make_presets()
    {
        DarwinAbiPreset executable { "mach-threads-executable", { }, { } };
        executable.abi.abi_epoch = DarwinAbiEpoch::IphoneOs1;
        executable.abi.capabilities = { true, true, true };
        executable.abi.activation_hardware_model_policy =
            ActivationHardwareModelPolicy::DevelopmentBoard;

        auto disk_policy = executable;
        disk_policy.name = "mach-threads-disk-policy";
        disk_policy.abi.abi_epoch = DarwinAbiEpoch::IphoneOs2;
        disk_policy.abi.capabilities = { true, false, false };

        auto shared_cache = disk_policy;
        shared_cache.name = "mach-threads-shared-cache";
        shared_cache.abi.abi_epoch = DarwinAbiEpoch::IphoneOs3;

        auto bootstrap_notify = shared_cache;
        bootstrap_notify.name = "bsd-threads-bootstrap-notify";
        bootstrap_notify.abi.abi_epoch = DarwinAbiEpoch::Darwin10;
        bootstrap_notify.abi.pthread_abi = DarwinPthreadAbi::BsdThreadRegisterV1;
        bootstrap_notify.abi.apple80211_ioctl =
            DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord;
        bootstrap_notify.abi.notify_state_abi =
            DarwinNotifyStateAbi::BootstrapAwareServerTokens;
        bootstrap_notify.abi.psynch_abi = DarwinPsynchAbi::Arm32GenerationV1;
        bootstrap_notify.identity.name = "darwin10.3-arm";
        bootstrap_notify.identity.operating_system_release = "10.3.1";
        bootstrap_notify.identity.version =
            "Darwin Kernel Version 10.3.1: iLEmu compatibility kernel; "
            "darwin10.3/RELEASE_ARM";

        auto legacy_apple = bootstrap_notify;
        legacy_apple.name = "bsd-threads-legacy-apple";
        legacy_apple.abi.notify_state_abi = DarwinNotifyStateAbi::NativeServerTokens;
        legacy_apple.abi.initial_apple_vector_abi =
            DarwinInitialAppleVectorAbi::LegacyExecutablePath;
        legacy_apple.identity.name = "darwin10.3-arm-v1";

        auto embedded_tsd = legacy_apple;
        embedded_tsd.name = "bsd-threads-embedded-tsd";
        embedded_tsd.abi.pthread_abi = DarwinPthreadAbi::BsdThreadRegisterV1TsdBase;
        embedded_tsd.identity.name = "darwin10.4-arm-v1-tsd";
        embedded_tsd.identity.operating_system_release = "10.4.0";
        embedded_tsd.identity.version =
            "Darwin Kernel Version 10.4.0: iLEmu compatibility kernel; "
            "darwin10.4/RELEASE_ARM";

        auto inline_iokit = embedded_tsd;
        inline_iokit.name = "bsd-threads-inline-iokit";
        inline_iokit.abi.abi_epoch = DarwinAbiEpoch::Darwin11;
        inline_iokit.abi.pthread_abi =
            DarwinPthreadAbi::BsdThreadRegisterV1TsdBaseFourPriorityWorkqueues;
        inline_iokit.abi.io_connect_method =
            DarwinIOConnectMethodAbi::Natural32OolStructureThenScalar;
        inline_iokit.abi.iokit_matching_rpc =
            DarwinIOKitMatchingRpcAbi::InlineSingleServiceV1;
        inline_iokit.identity.name = "darwin11.0-arm-v1-tsd";
        inline_iokit.identity.operating_system_release = "11.0.0";
        inline_iokit.identity.version =
            "Darwin Kernel Version 11.0.0: iLEmu compatibility kernel; "
            "darwin11.0/RELEASE_ARM";

        auto wide_vm = inline_iokit;
        wide_vm.name = "bsd-threads-wide-vm";
        wide_vm.abi.io_connect_method =
            DarwinIOConnectMethodAbi::MachVm64OolStructureThenScalar;
        wide_vm.abi.shared_region_abi =
            DarwinSharedRegionAbi::FixedMappingsWithSlideInfoV1;
        wide_vm.abi.mach_kernel_rpc = DarwinMachKernelRpcAbi::DirectVmAndPortTrapsV1;
        wide_vm.abi.activation_hardware_model_policy =
            ActivationHardwareModelPolicy::Retail;
        wide_vm.abi.mach_vm_address = DarwinMachVmAddressWidth::Wide64;
        wide_vm.identity.name = "darwin11.0-arm-v1-tsd-slide";

        auto register_v2 = disk_policy;
        register_v2.name = "bsd-threads-register-v2";
        register_v2.abi.abi_epoch = DarwinAbiEpoch::Later;
        register_v2.abi.pthread_abi = DarwinPthreadAbi::BsdThreadRegisterV2;
        register_v2.abi.shared_region_abi =
            DarwinSharedRegionAbi::FixedMappingsWithSlideInfoV1;
        register_v2.abi.mach_kernel_rpc =
            DarwinMachKernelRpcAbi::DirectVmAndPortTrapsV1;
        register_v2.abi.psynch_abi = DarwinPsynchAbi::Arm32GenerationV1;
        register_v2.abi.activation_hardware_model_policy =
            ActivationHardwareModelPolicy::Retail;
        register_v2.abi.mach_vm_address = DarwinMachVmAddressWidth::Wide64;

        return std::array { executable, disk_policy, shared_cache,
            bootstrap_notify, legacy_apple, embedded_tsd, inline_iokit,
            wide_vm, register_v2 };
    }

    const auto presets = make_presets();

    const DarwinAbiPreset* find_preset(std::string_view name)
    {
        const auto found = std::find_if(presets.begin(), presets.end(),
            [name](const auto& preset) { return preset.name == name; });
        return found == presets.end() ? nullptr : &*found;
    }

    // Transitional recognition for existing automatic callers. The actual
    // contracts above have no firmware build key; explicit selection bypasses
    // this adapter. Binary evidence can replace it independently of dispatch.
    const DarwinAbiPreset* legacy_metadata_preset(std::string_view build)
    {
        struct Rule { std::string_view prefix; std::string_view abi_name; };
        constexpr std::array rules {
            Rule { "1A", "mach-threads-executable" },
            Rule { "3A", "mach-threads-executable" },
            Rule { "4B", "mach-threads-executable" },
            Rule { "5A", "mach-threads-disk-policy" },
            Rule { "5G", "mach-threads-disk-policy" },
            Rule { "7A", "mach-threads-shared-cache" },
            Rule { "7B", "bsd-threads-bootstrap-notify" },
            Rule { "8A", "bsd-threads-legacy-apple" },
            Rule { "8C", "bsd-threads-embedded-tsd" },
            Rule { "8F", "bsd-threads-inline-iokit" },
            Rule { "9A", "bsd-threads-wide-vm" },
            Rule { "11", "bsd-threads-register-v2" },
        };
        for (const auto& rule : rules) {
            if (build.starts_with(rule.prefix))
                return find_preset(rule.abi_name);
        }
        return nullptr;
    }

} // namespace

std::span<const DarwinAbiPreset> darwin_abi_presets() { return presets; }

std::string_view darwin_abi_source_name(DarwinAbiSource source)
{
    switch (source) {
    case DarwinAbiSource::CompiledDefault: return "compiled-default";
    case DarwinAbiSource::FirmwareMetadata: return "firmware-metadata";
    case DarwinAbiSource::Explicit: return "explicit";
    case DarwinAbiSource::Unresolved: return "unresolved";
    }
    return "unresolved";
}

DarwinKernelConfiguration resolve_darwin_configuration(
    const std::filesystem::path& rootfs, std::string_view requested_abi)
{
    DarwinKernelConfiguration configuration;
    const auto build = read_darwin_build_version(rootfs);
    const DarwinAbiPreset* selected = nullptr;
    if (!requested_abi.empty() && requested_abi != "auto") {
        selected = find_preset(requested_abi);
        if (selected == nullptr) {
            throw std::runtime_error { "unknown ABI contract: " +
                                       std::string { requested_abi } +
                                       "; use 'ilemu abi' to list contracts" };
        }
        configuration.abi_source = DarwinAbiSource::Explicit;
        configuration.abi_source_detail = requested_abi;
    } else if (!build.empty()) {
        selected = legacy_metadata_preset(build);
        configuration.abi_source = selected ? DarwinAbiSource::FirmwareMetadata
                                           : DarwinAbiSource::Unresolved;
        configuration.abi_source_detail = build;
    } else if (rootfs.empty() || !std::filesystem::exists(rootfs)) {
        selected = find_preset("mach-threads-executable");
        configuration.abi_source = DarwinAbiSource::CompiledDefault;
    }
    if (selected != nullptr) {
        configuration.abi = selected->abi;
        configuration.identity = selected->identity;
        configuration.abi_name = selected->name;
    }
    if (!build.empty())
        configuration.identity.build_version = build;
    return configuration;
}

} // namespace ilemu
