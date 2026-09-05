#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "device_state/lockdown_state.hpp"
#include "foundation/device_model.hpp"
#include "graphics/gles_renderer.hpp"
#include "network/host_network.hpp"

namespace ilemu {

enum class JitProfileMode : std::uint8_t {
    Adaptive,
    Off,
    RecordOnly,
    LoadOnly,
    Idle,
    Startup,
};

enum class JitCatalogWarmingMode : std::uint8_t { NoEnqueue };

// Frontends parse syntax; the session owns execution and resource policies.
// Optional limits distinguish explicit overrides from adaptive defaults.
struct BootOptions {
    std::filesystem::path rootfs;
    std::filesystem::path host_cache;
    std::optional<std::string> catalog;
    DeviceModel device { DeviceModel::default_model() };
    std::optional<DisplayGeometry> display_geometry;
    std::string binary { "/sbin/launchd" };
    std::optional<std::string> guest_command;
    LockdownActivation activation { LockdownActivation::Activated };
    HostNetworkPolicy network { HostNetworkPolicy::Host };
    GlesBackend gles_backend { GlesBackend::Auto };
    bool windowed { };
    bool quiet_output { };
    bool control_enabled { };
    bool disable_scheduler_preemption { };
    bool jit_observer_only { };
    bool report_performance { };
    std::optional<std::uint64_t> ticks;
    std::optional<std::size_t> cores;
    std::optional<std::size_t> jit_cache_bytes;
    std::optional<std::size_t> jit_cache_budget_bytes;
    std::size_t artifact_memory_bytes { 64U * 1024U * 1024U };
    std::optional<std::size_t> artifact_disk_bytes;
    JitProfileMode jit_profile_mode { JitProfileMode::Adaptive };
    JitCatalogWarmingMode jit_catalog_warming {
        JitCatalogWarmingMode::NoEnqueue
    };
    std::uint64_t startup_profile_blocks { 64 };
    std::uint64_t startup_profile_budget_us { 4'000 };
    std::optional<std::string> frame_output;
    std::optional<std::string> touch_replay;
    std::optional<std::uint16_t> gdb_port;
    std::optional<std::uint32_t> watch_address;
    std::optional<std::string> baseband_input;
    std::optional<std::string> baseband_output;
};

[[nodiscard]] std::filesystem::path default_host_cache_directory(
    const std::filesystem::path& rootfs);

} // namespace ilemu
