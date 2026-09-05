#include "device_state/darwin_kernel_identity.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#if defined(ILEMU_HAS_LIBPLIST)
#include <plist/plist.h>
#endif

namespace ilemu {
namespace {

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream input { path, std::ios::binary };
        if (!input)
            return { };
        return { std::istreambuf_iterator<char> { input },
            std::istreambuf_iterator<char> { } };
    }

    std::optional<std::string> xml_string(
        std::string_view xml, std::string_view key)
    {
        const auto encoded_key = "<key>" + std::string { key } + "</key>";
        const auto key_position = xml.find(encoded_key);
        if (key_position == std::string_view::npos)
            return std::nullopt;
        constexpr std::string_view opening { "<string>" };
        constexpr std::string_view closing { "</string>" };
        const auto value_position =
            xml.find(opening, key_position + encoded_key.size());
        if (value_position == std::string_view::npos)
            return std::nullopt;
        const auto value_begin = value_position + opening.size();
        const auto value_end = xml.find(closing, value_begin);
        if (value_end == std::string_view::npos)
            return std::nullopt;
        return std::string { xml.substr(value_begin, value_end - value_begin) };
    }

    struct SystemVersion {
        std::string build_version;
    };

    enum class BuildMatchKind : std::uint8_t { FamilyPrefix };

    struct BuildContractRule {
        std::string_view matcher;
        BuildMatchKind match_kind;
        DarwinAbiEpoch abi_epoch;
        DarwinPthreadAbi pthread_abi;
        DarwinApple80211IoctlAbi apple80211_ioctl;
        DarwinIOConnectMethodAbi io_connect_method;
        DarwinNotifyStateAbi notify_state_abi;
        DarwinInitialAppleVectorAbi initial_apple_vector_abi;
        DarwinGuestCapabilities capabilities;
        std::string_view contract_name;
        std::string_view operating_system_release;
        std::uint32_t operating_system_revision { };
        std::string_view kernel_version;
        DarwinSharedRegionAbi shared_region_abi {
            DarwinSharedRegionAbi::LegacyRelocatableMappings
        };
        DarwinMachKernelRpcAbi mach_kernel_rpc {
            DarwinMachKernelRpcAbi::LegacyMigOnly
        };
        DarwinPsynchAbi psynch_abi {
            DarwinPsynchAbi::Unsupported
        };
        DarwinIOKitMatchingRpcAbi iokit_matching_rpc {
            DarwinIOKitMatchingRpcAbi::PluralIteratorOnly
        };
        ActivationHardwareModelPolicy activation_hardware_model_policy {
            ActivationHardwareModelPolicy::DevelopmentBoard
        };
        DarwinMachVmAddressWidth mach_vm_address {
            DarwinMachVmAddressWidth::Natural32
        };
    };

    constexpr BuildContractRule build_family_rule(std::string_view matcher,
        DarwinAbiEpoch abi_epoch, DarwinPthreadAbi pthread_abi,
        DarwinGuestCapabilities capabilities,
        DarwinSharedRegionAbi shared_region_abi =
            DarwinSharedRegionAbi::LegacyRelocatableMappings,
        DarwinMachKernelRpcAbi mach_kernel_rpc =
            DarwinMachKernelRpcAbi::LegacyMigOnly,
        DarwinPsynchAbi psynch_abi = DarwinPsynchAbi::Unsupported,
        DarwinIOKitMatchingRpcAbi iokit_matching_rpc =
            DarwinIOKitMatchingRpcAbi::PluralIteratorOnly,
        ActivationHardwareModelPolicy activation_hardware_model_policy =
            ActivationHardwareModelPolicy::DevelopmentBoard,
        DarwinMachVmAddressWidth mach_vm_address =
            DarwinMachVmAddressWidth::Natural32)
    {
        return { matcher, BuildMatchKind::FamilyPrefix, abi_epoch, pthread_abi,
            DarwinApple80211IoctlAbi::AlignedCurrentNetworkRecord,
            DarwinIOConnectMethodAbi::Natural32OolScalarThenStructure,
            DarwinNotifyStateAbi::NativeServerTokens,
            DarwinInitialAppleVectorAbi::KeyedExecutablePath, capabilities,
            { }, { }, 0, { }, shared_region_abi, mach_kernel_rpc, psynch_abi,
            iokit_matching_rpc, activation_hardware_model_policy,
            mach_vm_address };
    }

    // Keep build recognition data-driven at the ABI-family boundary. Individual
    // firmware releases within an audited family share the same contract; the
    // dispatchers consume only the resulting epoch and capabilities. The final
    // rule is an audited build-series prefix for the extended compatibility
    // contract, not a catch-all for arbitrary numeric builds.
    constexpr std::array build_contract_rules {
        build_family_rule("1A", DarwinAbiEpoch::IphoneOs1,
            DarwinPthreadAbi::LegacyMachThreads, { true, true, true }),
        build_family_rule("3A", DarwinAbiEpoch::IphoneOs1,
            DarwinPthreadAbi::LegacyMachThreads, { true, true, true }),
        build_family_rule("4B", DarwinAbiEpoch::IphoneOs1,
            DarwinPthreadAbi::LegacyMachThreads, { true, true, true }),
        build_family_rule("5A", DarwinAbiEpoch::IphoneOs2,
            DarwinPthreadAbi::LegacyMachThreads, { true, false, false }),
        build_family_rule("5G", DarwinAbiEpoch::IphoneOs2,
            DarwinPthreadAbi::LegacyMachThreads, { true, false, false }),
        build_family_rule("7A", DarwinAbiEpoch::IphoneOs3,
            DarwinPthreadAbi::LegacyMachThreads, { true, false, false }),
        BuildContractRule { "7B", BuildMatchKind::FamilyPrefix,
            DarwinAbiEpoch::Darwin10,
            DarwinPthreadAbi::BsdThreadRegisterV1,
            DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord,
            DarwinIOConnectMethodAbi::Natural32OolScalarThenStructure,
            DarwinNotifyStateAbi::BootstrapAwareServerTokens,
            DarwinInitialAppleVectorAbi::KeyedExecutablePath,
            { true, false, false }, "darwin10.3-arm", "10.3.1", 199506,
            "Darwin Kernel Version 10.3.1: iLEmu compatibility kernel; "
            "darwin10.3/RELEASE_ARM",
            DarwinSharedRegionAbi::LegacyRelocatableMappings,
            DarwinMachKernelRpcAbi::LegacyMigOnly,
            DarwinPsynchAbi::Arm32GenerationV1,
            DarwinIOKitMatchingRpcAbi::PluralIteratorOnly,
            ActivationHardwareModelPolicy::DevelopmentBoard },
        BuildContractRule { "8A", BuildMatchKind::FamilyPrefix,
            DarwinAbiEpoch::Darwin10,
            DarwinPthreadAbi::BsdThreadRegisterV1,
            DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord,
            DarwinIOConnectMethodAbi::Natural32OolScalarThenStructure,
            DarwinNotifyStateAbi::NativeServerTokens,
            DarwinInitialAppleVectorAbi::LegacyExecutablePath,
            { true, false, false }, "darwin10.3-arm-v1", "10.3.1", 199506,
            "Darwin Kernel Version 10.3.1: iLEmu compatibility kernel; "
            "darwin10.3/RELEASE_ARM",
            DarwinSharedRegionAbi::LegacyRelocatableMappings,
            DarwinMachKernelRpcAbi::LegacyMigOnly,
            DarwinPsynchAbi::Arm32GenerationV1,
            DarwinIOKitMatchingRpcAbi::PluralIteratorOnly,
            ActivationHardwareModelPolicy::DevelopmentBoard },
        BuildContractRule { "8C", BuildMatchKind::FamilyPrefix,
            DarwinAbiEpoch::Darwin10,
            DarwinPthreadAbi::BsdThreadRegisterV1TsdBase,
            DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord,
            DarwinIOConnectMethodAbi::Natural32OolScalarThenStructure,
            DarwinNotifyStateAbi::NativeServerTokens,
            DarwinInitialAppleVectorAbi::LegacyExecutablePath,
            { true, false, false }, "darwin10.4-arm-v1-tsd", "10.4.0", 199506,
            "Darwin Kernel Version 10.4.0: iLEmu compatibility kernel; "
            "darwin10.4/RELEASE_ARM",
            DarwinSharedRegionAbi::LegacyRelocatableMappings,
            DarwinMachKernelRpcAbi::LegacyMigOnly,
            DarwinPsynchAbi::Arm32GenerationV1,
            DarwinIOKitMatchingRpcAbi::PluralIteratorOnly,
            ActivationHardwareModelPolicy::DevelopmentBoard },
        BuildContractRule { "8F", BuildMatchKind::FamilyPrefix,
            DarwinAbiEpoch::Darwin11,
            DarwinPthreadAbi::
                BsdThreadRegisterV1TsdBaseFourPriorityWorkqueues,
            DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord,
            DarwinIOConnectMethodAbi::Natural32OolStructureThenScalar,
            DarwinNotifyStateAbi::NativeServerTokens,
            DarwinInitialAppleVectorAbi::LegacyExecutablePath,
            { true, false, false }, "darwin11.0-arm-v1-tsd", "11.0.0", 199506,
            "Darwin Kernel Version 11.0.0: iLEmu compatibility kernel; "
            "darwin11.0/RELEASE_ARM",
            DarwinSharedRegionAbi::LegacyRelocatableMappings,
            DarwinMachKernelRpcAbi::LegacyMigOnly,
            DarwinPsynchAbi::Arm32GenerationV1,
            DarwinIOKitMatchingRpcAbi::InlineSingleServiceV1,
            ActivationHardwareModelPolicy::DevelopmentBoard },
        BuildContractRule { "9A", BuildMatchKind::FamilyPrefix,
            DarwinAbiEpoch::Darwin11,
            DarwinPthreadAbi::
                BsdThreadRegisterV1TsdBaseFourPriorityWorkqueues,
            DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord,
            DarwinIOConnectMethodAbi::MachVm64OolStructureThenScalar,
            DarwinNotifyStateAbi::NativeServerTokens,
            DarwinInitialAppleVectorAbi::LegacyExecutablePath,
            { true, false, false }, "darwin11.0-arm-v1-tsd-slide", "11.0.0",
            199506,
            "Darwin Kernel Version 11.0.0: iLEmu compatibility kernel; "
            "darwin11.0/RELEASE_ARM",
            DarwinSharedRegionAbi::FixedMappingsWithSlideInfoV1,
            DarwinMachKernelRpcAbi::DirectVmAndPortTrapsV1,
            DarwinPsynchAbi::Arm32GenerationV1,
            DarwinIOKitMatchingRpcAbi::InlineSingleServiceV1,
            ActivationHardwareModelPolicy::Retail,
            DarwinMachVmAddressWidth::Wide64 },
        build_family_rule("11", DarwinAbiEpoch::Later,
            DarwinPthreadAbi::BsdThreadRegisterV2,
            { true, false, false },
            DarwinSharedRegionAbi::FixedMappingsWithSlideInfoV1,
            DarwinMachKernelRpcAbi::DirectVmAndPortTrapsV1,
            DarwinPsynchAbi::Arm32GenerationV1,
            DarwinIOKitMatchingRpcAbi::PluralIteratorOnly,
            ActivationHardwareModelPolicy::Retail,
            DarwinMachVmAddressWidth::Wide64),
    };

    [[nodiscard]] bool matches_build(
        const BuildContractRule& rule, std::string_view build)
    {
        switch (rule.match_kind) {
        case BuildMatchKind::FamilyPrefix:
            return build.starts_with(rule.matcher);
        }
        return false;
    }

    struct DarwinBuildContract {
        DarwinAbiEpoch abi_epoch { DarwinAbiEpoch::Unknown };
        DarwinPthreadAbi pthread_abi {
            DarwinPthreadAbi::LegacyMachThreads
        };
        DarwinApple80211IoctlAbi apple80211_ioctl {
            DarwinApple80211IoctlAbi::AlignedCurrentNetworkRecord
        };
        DarwinIOConnectMethodAbi io_connect_method {
            DarwinIOConnectMethodAbi::Natural32OolScalarThenStructure
        };
        DarwinNotifyStateAbi notify_state_abi {
            DarwinNotifyStateAbi::NativeServerTokens
        };
        DarwinInitialAppleVectorAbi initial_apple_vector_abi {
            DarwinInitialAppleVectorAbi::KeyedExecutablePath
        };
        DarwinGuestCapabilities capabilities { };
        std::string_view contract_name;
        std::string_view operating_system_release;
        std::uint32_t operating_system_revision { };
        std::string_view kernel_version;
        DarwinSharedRegionAbi shared_region_abi {
            DarwinSharedRegionAbi::LegacyRelocatableMappings
        };
        DarwinMachKernelRpcAbi mach_kernel_rpc {
            DarwinMachKernelRpcAbi::LegacyMigOnly
        };
        DarwinPsynchAbi psynch_abi {
            DarwinPsynchAbi::Unsupported
        };
        DarwinIOKitMatchingRpcAbi iokit_matching_rpc {
            DarwinIOKitMatchingRpcAbi::PluralIteratorOnly
        };
        ActivationHardwareModelPolicy activation_hardware_model_policy {
            ActivationHardwareModelPolicy::Retail
        };
        DarwinMachVmAddressWidth mach_vm_address {
            DarwinMachVmAddressWidth::Natural32
        };
    };

    [[nodiscard]] DarwinBuildContract contract_for_build(std::string_view build)
    {
        for (const auto& rule : build_contract_rules) {
            if (matches_build(rule, build))
                return { rule.abi_epoch, rule.pthread_abi,
                    rule.apple80211_ioctl, rule.io_connect_method,
                    rule.notify_state_abi,
                    rule.initial_apple_vector_abi, rule.capabilities,
                    rule.contract_name, rule.operating_system_release,
                    rule.operating_system_revision, rule.kernel_version,
                    rule.shared_region_abi, rule.mach_kernel_rpc,
                    rule.psynch_abi, rule.iokit_matching_rpc,
                    rule.activation_hardware_model_policy,
                    rule.mach_vm_address };
        }
        // Unknown epochs intentionally expose no version-sensitive capability.
        // Additive and shape-dispatched routes remain available through their
        // route metadata, while ambiguous calls receive deterministic safe
        // errors.
        return { };
    }

    SystemVersion read_system_version(const std::filesystem::path& rootfs)
    {
        const auto bytes = read_file(
            rootfs / "System/Library/CoreServices/SystemVersion.plist");
        SystemVersion result;
#if defined(ILEMU_HAS_LIBPLIST)
        plist_t parsed = nullptr;
        plist_format_t format = PLIST_FORMAT_NONE;
        if (!bytes.empty() &&
            plist_from_memory(bytes.data(),
                static_cast<std::uint32_t>(bytes.size()), &parsed,
                &format) == PLIST_ERR_SUCCESS &&
            parsed != nullptr && plist_get_node_type(parsed) == PLIST_DICT) {
            const auto read_string = [parsed](const char* key) {
                const auto node = plist_dict_get_item(parsed, key);
                if (node == nullptr ||
                    plist_get_node_type(node) != PLIST_STRING)
                    return std::string { };
                std::uint64_t length { };
                const auto* value = plist_get_string_ptr(node, &length);
                return value == nullptr
                           ? std::string { }
                           : std::string { value,
                                 static_cast<std::size_t>(length) };
            };
            result.build_version = read_string("ProductBuildVersion");
        }
        if (parsed != nullptr)
            plist_free(parsed);
#endif
        if (result.build_version.empty()) {
            result.build_version =
                xml_string(bytes, "ProductBuildVersion").value_or("");
        }
        return result;
    }

} // namespace

DarwinKernelIdentity make_darwin_kernel_identity(
    const std::filesystem::path& rootfs)
{
    const auto system_version = read_system_version(rootfs);
    DarwinKernelIdentity identity;
    if (!system_version.build_version.empty()) {
        identity.build_version = system_version.build_version;
        identity.abi_build_version = system_version.build_version;
    } else if (rootfs.empty() || !std::filesystem::exists(rootfs)) {
        // Unit and embedding callers may intentionally omit a firmware rootfs.
        // In that case use the explicitly compiled compatibility default rather
        // than treating the absence of a fixture as an unidentified firmware.
        identity.abi_build_version = identity.build_version;
    }
    const auto contract = contract_for_build(identity.abi_build_version);
    identity.abi_epoch = contract.abi_epoch;
    identity.pthread_abi = contract.pthread_abi;
    identity.apple80211_ioctl = contract.apple80211_ioctl;
    identity.io_connect_method = contract.io_connect_method;
    identity.mach_vm_address = contract.mach_vm_address;
    identity.notify_state_abi = contract.notify_state_abi;
    identity.initial_apple_vector_abi =
        contract.initial_apple_vector_abi;
    identity.shared_region_abi = contract.shared_region_abi;
    identity.mach_kernel_rpc = contract.mach_kernel_rpc;
    identity.psynch_abi = contract.psynch_abi;
    identity.iokit_matching_rpc = contract.iokit_matching_rpc;
    identity.activation_hardware_model_policy =
        contract.activation_hardware_model_policy;
    identity.capabilities = contract.capabilities;
    if (!contract.contract_name.empty())
        identity.name = contract.contract_name;
    if (!contract.operating_system_release.empty())
        identity.operating_system_release = contract.operating_system_release;
    if (contract.operating_system_revision != 0)
        identity.operating_system_revision = contract.operating_system_revision;
    if (!contract.kernel_version.empty())
        identity.version = contract.kernel_version;
    return identity;
}

} // namespace ilemu
