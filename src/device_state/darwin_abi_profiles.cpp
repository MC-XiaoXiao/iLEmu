// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Ordered ARM32 userland contracts. Each release changes only the wire
// contracts that differ from its predecessor. Build selection lives here;
// kernel handlers consume the resolved contracts without parsing versions.

#include "darwin_abi_profiles.hpp"

#include <array>

namespace ilemu {
namespace {

    constexpr DarwinAbi iphoneos_1 = [] {
        DarwinAbi abi;
        abi.abi_epoch = DarwinAbiEpoch::IphoneOs1;
        abi.mach_descriptor_copyin =
            DarwinMachDescriptorCopyinAbi::ReverseCompactDescriptors;
        abi.capabilities.send_sigsys = true;
        abi.capabilities.arm_cache_trap_grants_execute = true;
        return abi;
    }();

    constexpr DarwinAbi iphoneos_2 = [] {
        auto abi = iphoneos_1;
        abi.abi_epoch = DarwinAbiEpoch::IphoneOs2;
        abi.task_information = DarwinTaskInformationAbi::ResidentPeak;
        abi.child_wait = DarwinChildWaitAbi::ExpandedStopMask;
        abi.task_special_ports =
            DarwinTaskSpecialPortsAbi::SecurityLedgerAutomount;
        abi.sysctl_transfer = DarwinSysctlTransferAbi::OidSizedQueries;
        abi.capabilities.arm_cache_trap_grants_execute = false;
        return abi;
    }();

    // Only incompatible wire revisions retain train boundaries: syscall slots
    // 299/300 change shared-region -> psynch; IOKit output ordering and the
    // pthread TSD layout also change within a major userland generation.
    constexpr DarwinAbi iphoneos_3 = [] {
        auto abi = iphoneos_2;
        abi.abi_epoch = DarwinAbiEpoch::IphoneOs3;
        abi.mach_descriptor_copyin =
            DarwinMachDescriptorCopyinAbi::ForwardExpandedDescriptors;
        abi.task_information = DarwinTaskInformationAbi::DyldAddressPair;
        abi.resource_accounting = DarwinResourceAccounting::ResidentPeak;
        abi.task_special_ports = DarwinTaskSpecialPortsAbi::SecurityLedger;
        return abi;
    }();

    constexpr DarwinAbi iphoneos_3_1 = [] {
        auto abi = iphoneos_3;
        abi.abi_epoch = DarwinAbiEpoch::Darwin10;
        abi.file_open = DarwinFileOpenAbi::DataSynchronized;
        abi.descriptor_passing = DarwinDescriptorPassingAbi::SendableFileTypes;
        abi.sysctl_transfer = DarwinSysctlTransferAbi::OidNaturalSizeQueries;
        abi.pthread_abi = DarwinPthreadAbi::BsdThreadRegisterV1;
        abi.apple80211_ioctl =
            DarwinApple80211IoctlAbi::CompactCurrentNetworkRecord;
        abi.notify_state_abi = DarwinNotifyStateAbi::BootstrapAwareServerTokens;
        abi.psynch_abi = DarwinPsynchAbi::Arm32GenerationV1;
        abi.io_service_state = DarwinIOServiceStateAbi::StateWithBusyAccounting;
        return abi;
    }();

    constexpr DarwinAbi ios_4 = [] {
        auto abi = iphoneos_3_1;
        abi.notify_state_abi = DarwinNotifyStateAbi::NativeServerTokens;
        abi.initial_apple_vector_abi =
            DarwinInitialAppleVectorAbi::LegacyExecutablePath;
        abi.semaphore_wait_abi = DarwinSemaphoreWaitAbi::InlineSeconds64;
        return abi;
    }();

    constexpr DarwinAbi ios_4_2 = [] {
        auto abi = ios_4;
        abi.pthread_abi = DarwinPthreadAbi::BsdThreadRegisterV1TsdBase;
        return abi;
    }();

    constexpr DarwinAbi ios_4_3 = [] {
        auto abi = ios_4_2;
        abi.abi_epoch = DarwinAbiEpoch::Darwin11;
        abi.task_information = DarwinTaskInformationAbi::DyldWithFormat;
        abi.mach_port_requests = DarwinMachPortRequestAbi::SendPossibleRequests;
        abi.socket_accept_flags = DarwinSocketAcceptFlagsAbi::ExtendedFlags;
        abi.file_open = DarwinFileOpenAbi::CloseOnExec;
        abi.thread_priority_floor = DarwinThreadPriorityFloor::Throttle;
        abi.pthread_abi =
            DarwinPthreadAbi::BsdThreadRegisterV1TsdBaseFourPriorityWorkqueues;
        abi.io_connect_method =
            DarwinIOConnectMethodAbi::Natural32OolStructureThenScalar;
        abi.psynch_rw_conversion = DarwinPsynchRwConversionAbi::UnusedStubs;
        abi.stack_snapshot_abi = DarwinStackSnapshotAbi::LegacyFourArguments;
        abi.iokit_matching_rpc =
            DarwinIOKitMatchingRpcAbi::InlineSingleServiceV1;
        return abi;
    }();

    constexpr DarwinAbi ios_5 = [] {
        auto abi = ios_4_3;
        abi.child_wait = DarwinChildWaitAbi::ChildIdentity;
        abi.io_connect_method =
            DarwinIOConnectMethodAbi::MachVm64OolStructureThenScalar;
        abi.mach_vm_address = DarwinMachVmAddressWidth::Wide64;
        abi.shared_region_abi =
            DarwinSharedRegionAbi::FixedMappingsWithSlideInfoV1;
        abi.mach_kernel_rpc = DarwinMachKernelRpcAbi::DirectVmAndPortTrapsV1;
        // install_commpage already retains the legacy address as an alias.
        abi.arm_commpage = DarwinArmCommpageAbi::HighAddress;
        return abi;
    }();

    constexpr DarwinAbi ios_6 = [] {
        auto abi = ios_5;
        abi.abi_epoch = DarwinAbiEpoch::Darwin13;
        abi.task_suspension = DarwinTaskSuspensionAbi::ProtectedPidHold;
        abi.task_information = DarwinTaskInformationAbi::UnifiedBasic;
        abi.task_special_ports = DarwinTaskSpecialPortsAbi::Security;
        abi.mach_port_ownership = DarwinMachPortOwnershipAbi::TemporaryReceiver;
        abi.socket_accept_failure = DarwinSocketAcceptFailureAbi::Discard;
        abi.iokit_matching_rpc =
            DarwinIOKitMatchingRpcAbi::InlineSingleServiceAfterVariableOutput;
        abi.sandbox_abi = DarwinSandboxAbi::Wide64Arguments;
        abi.mach_port_context = DarwinMachVmAddressWidth::Wide64;
        abi.memory_status_priority =
            DarwinMemoryStatusPriorityAbi::SignedPriority;
        abi.hid_digitizer = DarwinHidDigitizerAbi::ChildContactChanges;
        return abi;
    }();

    constexpr DarwinAbi ios_7 = [] {
        auto abi = ios_6;
        abi.abi_epoch = DarwinAbiEpoch::Later;
        abi.task_suspension = DarwinTaskSuspensionAbi::ResumePortTokens;
        abi.task_trace_memory = DarwinTaskTraceMemoryAbi::Registration;
        abi.mach_port_attribute_array =
            DarwinMachPortAttributeArray::ExtendedStatus17;
        abi.mach_port_status = DarwinMachPortStatusAbi::ImportanceAndGuards;
        abi.mach_message_header = DarwinMachMessageHeaderAbi::MaskedPortBytes;
        abi.thread_timeshare = DarwinThreadTimeshareAbi::CanonicalBoolean;
        abi.socket_linger = DarwinSocketLingerAbi::CanonicalBoolean;
        abi.thread_priority_floor = DarwinThreadPriorityFloor::Unrestricted;
        abi.pthread_abi = DarwinPthreadAbi::
            BsdThreadRegisterV1ExpandedTsdFourPriorityWorkqueues;
        abi.apple80211_ioctl =
            DarwinApple80211IoctlAbi::AlignedCurrentNetworkRecord;
        abi.arm_commpage = DarwinArmCommpageAbi::HighDataAddress;
        abi.mach_kernel_rpc = DarwinMachKernelRpcAbi::DirectWideVmAndPortTraps;
        abi.stack_snapshot_abi = DarwinStackSnapshotAbi::Unsupported;
        abi.framebuffer_registry =
            DarwinFramebufferRegistryAbi::UnifiedClcdClass;
        abi.memory_status_priority =
            DarwinMemoryStatusPriorityAbi::PriorityBands;
        abi.hid_digitizer =
            DarwinHidDigitizerAbi::CollectionContactChangesWithEdgeOrigin;
        abi.address_layout = DarwinAddressLayout::ExpandedArmSharedRegion;
        return abi;
    }();

    constexpr DarwinAbi ios_8 = [] {
        auto abi = ios_7;
        abi.task_trace_memory = DarwinTaskTraceMemoryAbi::ProcessInspection;
        abi.task_special_ports = DarwinTaskSpecialPortsAbi::SecurityDebug;
        abi.mach_message_header = DarwinMachMessageHeaderAbi::MaskedPortFields;
        abi.pthread_abi = DarwinPthreadAbi::
            BsdThreadRegisterV1ExpandedTsdFinePriorityWorkqueues;
        abi.sysctl_by_name_abi = DarwinSysctlByNameAbi::NamedSysctlAt274;
        abi.exception_port_abi = DarwinExceptionPortAbi::ThroughGuard;
        abi.coalition_abi = DarwinCoalitionAbi::ResourceCoalitions;
        abi.mach_voucher_abi =
            DarwinMachVoucherAbi::HostCreateWithInlineRecipes;
        abi.address_layout = DarwinAddressLayout::ArmSharedRegionAt512MiB;
        return abi;
    }();

    constexpr DarwinAbi ios_9 = [] {
        auto abi = ios_8;
        abi.exception_port_abi = DarwinExceptionPortAbi::ThroughCorpseNotify;
        abi.coalition_abi = DarwinCoalitionAbi::ResourceAndJetsamCoalitions;
        return abi;
    }();

    constexpr DarwinAbi ios_10 = [] {
        auto abi = ios_9;
        abi.gettimeofday_abi =
            DarwinGettimeofdayAbi::CopyoutTimevalAndAbsoluteTime;
        abi.pthread_abi =
            DarwinPthreadAbi::BsdThreadRegisterV1ExpandedTsdKeventWorkqueues;
        abi.mach_voucher_abi = DarwinMachVoucherAbi::HostCreateWithDirectTraps;
        abi.address_layout = DarwinAddressLayout::ArmSharedRegionAt416MiB;
        abi.capabilities.file_signature_end_offset = true;
        return abi;
    }();

    constexpr FirmwareActivationProfile development_activation {
        FirmwareActivationProfile::Identity::DevelopmentBoard
    };
    constexpr FirmwareActivationProfile retail_activation { };

    constexpr std::array configurations {
        DarwinConfigurationEntry { "iphoneos-1", "9.0.0d1", iphoneos_1, { 1U },
            development_activation },
        DarwinConfigurationEntry {
            "iphoneos-2", "9.4.1", iphoneos_2, { 5U }, development_activation },
        DarwinConfigurationEntry { "iphoneos-3", "10.0.0d3", iphoneos_3, { 7U },
            development_activation },
        DarwinConfigurationEntry { "iphoneos-3.1", "10.3.1", iphoneos_3_1,
            { 7U, 'B' }, development_activation },
        DarwinConfigurationEntry {
            "ios-4", "10.3.1", ios_4, { 8U }, development_activation },
        DarwinConfigurationEntry {
            "ios-4.2", "10.4.0", ios_4_2, { 8U, 'C' }, development_activation },
        DarwinConfigurationEntry {
            "ios-4.3", "11.0.0", ios_4_3, { 8U, 'F' }, development_activation },
        DarwinConfigurationEntry {
            "ios-5", "11.0.0", ios_5, { 9U }, retail_activation },
        DarwinConfigurationEntry {
            "ios-6", "13.0.0", ios_6, { 10U }, retail_activation },
        DarwinConfigurationEntry {
            "ios-7", "14.0.0", ios_7, { 11U }, retail_activation },
        DarwinConfigurationEntry {
            "ios-8", "14.0.0", ios_8, { 12U }, retail_activation },
        DarwinConfigurationEntry {
            "ios-9", "15.0.0", ios_9, { 13U }, retail_activation },
        DarwinConfigurationEntry { "ios-10", "16.0.0", ios_10, { 14U },
            retail_activation, UnlockInteraction::PressHome },
    };

    static_assert([] {
        for (std::size_t i = 1; i < configurations.size(); ++i) {
            if (configurations[i - 1].minimum_build >=
                configurations[i].minimum_build)
                return false;
        }
        return true;
    }());

} // namespace

std::span<const DarwinConfigurationEntry> darwin_configurations()
{
    return configurations;
}

const DarwinConfigurationEntry* darwin_configuration_for_build(
    DarwinBuildVersion build)
{
    // Later, unaudited major generations must not silently acquire the newest
    // known ABI. Within the supported range, inherit the last version boundary.
    if (!build.at_least(configurations.front().minimum_build) ||
        build.generation > configurations.back().minimum_build.generation)
        return nullptr;
    const DarwinConfigurationEntry* selected = nullptr;
    for (const auto& entry : configurations) {
        if (!build.at_least(entry.minimum_build))
            break;
        selected = &entry;
    }
    return selected;
}

} // namespace ilemu
