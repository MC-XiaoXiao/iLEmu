#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "ilemu/darwin_abi_route.hpp"
#include "ilemu/darwin_notify_state_hle.hpp"
#include "ilemu/darwin_process_start_profile.hpp"
#include "ilemu/device_profile.hpp"

namespace ilemu {

// libpthread moved thread creation and workqueue registration behind BSD
// syscalls in Darwin 10. Keep that wire contract separate from the broader
// kernel epoch: later Darwin releases revise the registration arguments and
// workqueue operations without changing the device model.
enum class DarwinPthreadAbiProfile : std::uint8_t {
    LegacyMachThreads,
    BsdThreadRegisterV1,
    // Darwin 10.4 ARM32 uses an embedded TSD base in pthread_t.
    BsdThreadRegisterV1TsdBase,
    // Darwin 11 retains the ARM32 v1 registration/TSD layout and adds the
    // background workqueue priority to high/default/low.
    BsdThreadRegisterV1TsdBaseFourPriorityWorkqueues,
    BsdThreadRegisterV2,
};

// Apple80211 keeps selector 201 across these releases, but the native driver
// record packed behind the ioctl changed independently of the public API.
// Name the two audited wire layouts so BSD emulation never needs to inspect a
// firmware build string or infer a layout from a caller-provided byte count.
enum class DarwinApple80211IoctlProfile : std::uint8_t {
    AlignedCurrentNetworkRecord,
    CompactCurrentNetworkRecord,
};

// The private io_connect_method request retained the same MIG routine number
// across revisions which independently reordered the output capacities and
// widened the out-of-line address/size fields to Mach VM values.
// Keep that wire detail independent of the user-client selector and broad
// kernel epoch so unknown firmware cannot be guessed from request contents.
enum class DarwinIOConnectMethodProfile : std::uint8_t {
    Natural32OolScalarThenStructure,
    Natural32OolStructureThenScalar,
    MachVm64OolStructureThenScalar,
};

// Address width changed within the Darwin 11 ARM32 family. It is independent
// of the kernel epoch and of vm_map's always-natural-sized address fields.
enum class DarwinMachVmAddressProfile : std::uint8_t {
    Natural32,
    Wide64,
};

// Later ARM32 firmware added a fixed mach_vm shared-region mapping array plus
// an optional dyld slide-info bitmap. Keep that wire contract independent of
// the broad kernel epoch: syscall numbers and argument meanings changed while
// the surrounding VM model remained stable.
enum class DarwinSharedRegionAbiProfile : std::uint8_t {
    LegacyRelocatableMappings,
    FixedMappingsWithSlideInfoV1,
};

// Some ARM32 libSystem releases use direct kernel-RPC traps for the hot Mach
// port operations that older releases send through MIG. Keep the selection
// explicit so a legacy guest never observes newer trap-table entries.
enum class DarwinMachKernelRpcProfile : std::uint8_t {
    LegacyMigOnly,
    DirectVmAndPortTrapsV1,
};

// Darwin 10 introduced the psynch syscall family in slots that older ARM32
// kernels used for shared-region operations.  Keep that syscall-table and
// argument-packing contract explicit so dispatch never guesses from a call's
// register contents or from an individual firmware build.
enum class DarwinPsynchAbiProfile : std::uint8_t {
    Unsupported,
    Arm32GenerationV1,
};

// Darwin 11's IOKit client added a private inline matching RPC which returns
// the first service directly instead of an iterator.  Its routine number and
// compact c-string request are independent of the registry contents, so keep
// the transport contract explicit and let all service classes share the same
// registry matcher.
enum class DarwinIOKitMatchingRpcProfile : std::uint8_t {
    PluralIteratorOnly,
    InlineSingleServiceV1,
};

// Offline activation was originally exposed through a firmware-supported
// development-board identity.  Later firmware route databases use the retail
// hardware identity even when the host supplies the activation state.  Keep
// that distinction in the firmware contract profile instead of deriving it
// from a product name or an application path.
using DarwinActivationHardwareModelProfile = ActivationHardwareModelPolicy;

struct DarwinGuestCapabilities {
    // XNU's nosys entry returns ENOSYS and raises SIGSYS on the audited
    // production epochs. Unknown profiles conservatively suppress the signal
    // until their kernel policy is identified.
    bool send_sigsys { };
    // Early UIKit emits writable ARM trampolines and relies on the emulator's
    // compatibility permission promotion after the instruction-cache trap.
    // This is a version-sensitive capability; later and unknown profiles keep
    // the XNU behavior of leaving VM permissions unchanged.
    bool arm_cache_trap_grants_execute { };
    // The early Lockdown ABI retains the platform serial property without
    // probing for its absence. Later audited families use the normal
    // kIOReturnNotFound contract, so the IOKit implementation consumes this
    // capability instead of matching firmware build strings.
    bool expose_legacy_platform_serial { };
};

struct DarwinKernelIdentityProfile {
    std::string name { "darwin9.4" };
    std::string operating_system_type { "Darwin" };
    std::string operating_system_release { "9.4.0" };
    std::uint32_t operating_system_revision { 199506 };
    std::string version {
        "Darwin Kernel Version 9.4.0: iLEmu compatibility kernel; "
        "darwin9.4/RELEASE_ARM"
    };
    // Compatibility value exposed through kern.osversion/sysctl when the
    // firmware does not provide a trustworthy ProductBuildVersion.
    std::string build_version { "1A543a" };
    // Empty means that a present firmware rootfs was missing, malformed, or did
    // not contain ProductBuildVersion. This is the only build string eligible
    // for version-sensitive ABI routing; callers that intentionally omit a
    // rootfs use the explicitly compiled compatibility default instead.
    std::string abi_build_version;
    DarwinAbiEpoch abi_epoch { DarwinAbiEpoch::Unknown };
    DarwinPthreadAbiProfile pthread_abi {
        DarwinPthreadAbiProfile::LegacyMachThreads
    };
    DarwinApple80211IoctlProfile apple80211_ioctl {
        DarwinApple80211IoctlProfile::AlignedCurrentNetworkRecord
    };
    DarwinIOConnectMethodProfile io_connect_method {
        DarwinIOConnectMethodProfile::Natural32OolScalarThenStructure
    };
    DarwinMachVmAddressProfile mach_vm_address {
        DarwinMachVmAddressProfile::Natural32
    };
    DarwinNotifyStateProfile notify_state_profile {
        DarwinNotifyStateProfile::NativeServerTokens
    };
    DarwinInitialAppleVectorProfile initial_apple_vector_profile {
        DarwinInitialAppleVectorProfile::KeyedExecutablePath
    };
    DarwinSharedRegionAbiProfile shared_region_abi {
        DarwinSharedRegionAbiProfile::LegacyRelocatableMappings
    };
    DarwinMachKernelRpcProfile mach_kernel_rpc {
        DarwinMachKernelRpcProfile::LegacyMigOnly
    };
    DarwinPsynchAbiProfile psynch_abi {
        DarwinPsynchAbiProfile::Unsupported
    };
    DarwinIOKitMatchingRpcProfile iokit_matching_rpc {
        DarwinIOKitMatchingRpcProfile::PluralIteratorOnly
    };
    DarwinActivationHardwareModelProfile activation_hardware_model_profile {
        DarwinActivationHardwareModelProfile::Retail
    };
    DarwinGuestCapabilities capabilities;
};

// Reports the compatibility kernel's highest supported Darwin contract. The
// detected ABI build is available to explicitly audited dispatch points;
// unknown builds retain the conservative compatibility behavior.
[[nodiscard]] DarwinKernelIdentityProfile make_darwin_kernel_identity_profile(
    const std::filesystem::path& rootfs);

} // namespace ilemu
