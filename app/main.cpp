#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <dynarmic/interface/A32/disassembler.h>

#include "ilemu/address_space.hpp"
#include "ilemu/application_path.hpp"
#include "ilemu/baseband_replay.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/deadline_queue.hpp"
#include "ilemu/device_profile.hpp"
#include "ilemu/display.hpp"
#include "ilemu/executable_catalog.hpp"
#include "ilemu/frame_file_presenter.hpp"
#include "ilemu/firmware_prepare.hpp"
#include "ilemu/gdb_rsp.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/guest_parallelism_policy.hpp"
#include "ilemu/host_file_watcher.hpp"
#include "ilemu/host_resource_controller.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/jit_artifact.hpp"
#include "ilemu/jit_translation_profile.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/live_control.hpp"
#include "ilemu/live_button_scheduler.hpp"
#include "ilemu/live_touch_scheduler.hpp"
#include "ilemu/lockdown_profile.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/macho.hpp"
#include "ilemu/network_preferences.hpp"
#include "ilemu/output.hpp"
#include "ilemu/performance.hpp"
#include "ilemu/process_loader.hpp"
#include "ilemu/realtime_pacer.hpp"
#include "ilemu/sdl_display.hpp"
#include "ilemu/touch_replay.hpp"
#include "ilemu/virtual_network.hpp"
#include "ilemu/wifi_state.hpp"
#include "ilemu/xnu_scheduler.hpp"
#include "ffmpeg_audio_decoder.hpp"
#include "sdl_audio_sink.hpp"

namespace {

using namespace ilemu;

constexpr std::size_t fault_stack_word_count = 32;
constexpr std::size_t maximum_watchpoint_traces = 64;
constexpr std::size_t initial_guest_thread_slots = 16;
constexpr std::size_t maximum_guest_threads = 32;
constexpr std::size_t maximum_virtual_processors = 64;
constexpr std::size_t maximum_shared_monitor_processes = 1024;
constexpr std::size_t maximum_shared_monitor_slots =
    maximum_virtual_processors * maximum_shared_monitor_processes;
constexpr std::size_t maximum_background_workers = 8;
constexpr std::size_t bytes_per_mebibyte = 1024U * 1024U;
constexpr std::size_t jit_minimum_shared_slab_bytes =
    8U * bytes_per_mebibyte;
constexpr std::size_t jit_emergency_budget_bytes =
    256U * bytes_per_mebibyte;
constexpr std::size_t jit_maximum_adaptive_budget_bytes =
    2048U * bytes_per_mebibyte;
constexpr std::size_t arm_thumb_breakpoint_size = 2;
constexpr std::size_t arm_breakpoint_size = 4;
// GDB and mixed SDL/control sessions still use this bounded fallback because
// those wrappers do not expose one waitable host descriptor. A standalone SDL
// session blocks directly on the SDL event queue.
constexpr auto sdl_event_poll_fallback = std::chrono::milliseconds{4};

struct HostMemorySnapshot {
  std::uint64_t rss_bytes{};
  std::uint64_t peak_rss_bytes{};
  std::uint64_t virtual_bytes{};
  std::uint64_t file_mapped_bytes{};
  bool rss_known{};
  bool peak_rss_known{};
  bool virtual_known{};
  bool file_mapped_known{};
};

[[nodiscard]] HostMemorySnapshot host_memory_snapshot() {
  HostMemorySnapshot snapshot;
#if defined(__linux__)
  std::ifstream status{ "/proc/self/status" };
  std::string line;
  while (std::getline(status, line)) {
    std::istringstream fields{line};
    std::string label;
    std::uint64_t value{};
    std::string unit;
    fields >> label >> value >> unit;
    if (!fields || unit != "kB") continue;
    if (value > std::numeric_limits<std::uint64_t>::max() / 1024U)
      continue;
    const auto bytes = value * 1024U;
    if (label == "VmRSS:") {
      snapshot.rss_bytes = bytes;
      snapshot.rss_known = true;
    }
    if (label == "VmHWM:") {
      snapshot.peak_rss_bytes = bytes;
      snapshot.peak_rss_known = true;
    }
    if (label == "VmSize:") {
      snapshot.virtual_bytes = bytes;
      snapshot.virtual_known = true;
    }
    if (label == "RssFile:") {
      snapshot.file_mapped_bytes = bytes;
      snapshot.file_mapped_known = true;
    }
  }
#endif
  return snapshot;
}

struct HostMemoryBudgetSnapshot {
  std::uint64_t physical_bytes{};
  std::uint64_t available_bytes{};
  std::uint64_t rss_bytes{};
  std::uint64_t cgroup_limit_bytes{};
  std::uint64_t cgroup_current_bytes{};
  bool physical_known{};
  bool available_known{};
  bool rss_known{};
  bool cgroup_limit_known{};
  bool cgroup_current_known{};
};

[[nodiscard]] std::optional<std::uint64_t> read_decimal_file(
    const std::filesystem::path& path) {
  std::ifstream input{path};
  std::string value;
  input >> value;
  if (!input || value.empty() || value == "max") return std::nullopt;
  std::size_t consumed{};
  try {
    const auto parsed = std::stoull(value, &consumed, 10);
    if (consumed != value.size()) return std::nullopt;
    return static_cast<std::uint64_t>(parsed);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

[[nodiscard]] HostMemoryBudgetSnapshot host_memory_budget_snapshot() {
  HostMemoryBudgetSnapshot snapshot;
  const auto process_memory = host_memory_snapshot();
  snapshot.rss_bytes = process_memory.rss_bytes;
  snapshot.rss_known = process_memory.rss_known;
#if defined(__linux__)
  {
    std::ifstream meminfo{"/proc/meminfo"};
    std::string line;
    while (std::getline(meminfo, line)) {
      std::istringstream fields{line};
      std::string label;
      std::uint64_t value{};
      std::string unit;
      fields >> label >> value >> unit;
      if (!fields || unit != "kB" ||
          value > std::numeric_limits<std::uint64_t>::max() / 1024U) {
        continue;
      }
      const auto bytes = value * 1024U;
      if (label == "MemTotal:") {
        snapshot.physical_bytes = bytes;
        snapshot.physical_known = true;
      }
      if (label == "MemAvailable:") {
        snapshot.available_bytes = bytes;
        snapshot.available_known = true;
      }
    }
  }

  std::filesystem::path cgroup_path;
  {
    std::ifstream groups{"/proc/self/cgroup"};
    std::string line;
    while (std::getline(groups, line)) {
      constexpr std::string_view unified_prefix = "0::";
      if (!line.starts_with(unified_prefix)) continue;
      auto relative = line.substr(unified_prefix.size());
      while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());
      cgroup_path = std::filesystem::path{"/sys/fs/cgroup"};
      if (!relative.empty()) cgroup_path /= relative;
      break;
    }
  }

  const auto read_first = [](const std::array<std::filesystem::path, 3>& paths)
      -> std::optional<std::uint64_t> {
    for (const auto& path : paths) {
      if (path.empty()) continue;
      if (const auto value = read_decimal_file(path)) return value;
    }
    return std::nullopt;
  };
  const std::array<std::filesystem::path, 3> limit_paths{
      cgroup_path.empty() ? std::filesystem::path{}
                          : cgroup_path / "memory.max",
      cgroup_path.empty() ? std::filesystem::path{}
                          : cgroup_path / "memory.limit_in_bytes",
      std::filesystem::path{"/sys/fs/cgroup/memory.max"},
  };
  const std::array<std::filesystem::path, 3> current_paths{
      cgroup_path.empty() ? std::filesystem::path{}
                          : cgroup_path / "memory.current",
      cgroup_path.empty() ? std::filesystem::path{}
                          : cgroup_path / "memory.usage_in_bytes",
      std::filesystem::path{"/sys/fs/cgroup/memory.current"},
  };
  if (const auto limit = read_first(limit_paths)) {
    snapshot.cgroup_limit_bytes = *limit;
    snapshot.cgroup_limit_known = true;
  }
  if (const auto current = read_first(current_paths)) {
    snapshot.cgroup_current_bytes = *current;
    snapshot.cgroup_current_known = true;
  }
  // cgroup-v1 uses a very large sentinel for "unlimited". Treat any limit
  // many times larger than physical memory as equivalent to no finite limit.
  if (snapshot.cgroup_limit_known && snapshot.physical_known &&
      snapshot.cgroup_limit_bytes != 0U && snapshot.physical_bytes != 0U &&
      snapshot.physical_bytes <=
          std::numeric_limits<std::uint64_t>::max() / 2U &&
      snapshot.cgroup_limit_bytes >
          snapshot.physical_bytes * std::uint64_t{2U}) {
    snapshot.cgroup_limit_bytes = 0U;
    snapshot.cgroup_limit_known = false;
  }
#endif
  return snapshot;
}

enum class HostMemoryPressureLevel : std::uint8_t {
  Unknown,
  Normal,
  Constrained,
  Critical,
};

[[nodiscard]] std::optional<std::uint64_t> effective_host_memory_limit(
    const HostMemoryBudgetSnapshot &memory) {
  std::optional<std::uint64_t> limit;
  if (memory.physical_known) limit = memory.physical_bytes;
  if (memory.cgroup_limit_known) {
    limit = limit ? std::min(*limit, memory.cgroup_limit_bytes)
                  : std::optional<std::uint64_t>{memory.cgroup_limit_bytes};
  }
  return limit;
}

[[nodiscard]] HostMemoryPressureLevel host_memory_pressure_level(
    const HostMemoryBudgetSnapshot &memory) {
  bool observed{};
  bool constrained{};
  bool critical{};
  if (memory.available_known) {
    observed = true;
    critical = critical || memory.available_bytes == 0U;
    if (memory.physical_known && memory.physical_bytes != 0U &&
        memory.available_bytes < memory.physical_bytes / 8U) {
      constrained = true;
    }
  }
  if (memory.cgroup_limit_known && memory.cgroup_current_known) {
    observed = true;
    if (memory.cgroup_current_bytes >= memory.cgroup_limit_bytes) {
      critical = true;
    } else if (memory.cgroup_limit_bytes != 0U &&
               memory.cgroup_limit_bytes - memory.cgroup_current_bytes <
                   memory.cgroup_limit_bytes / 8U) {
      constrained = true;
    }
  }
  if (const auto limit = effective_host_memory_limit(memory);
      limit && memory.rss_known) {
    observed = true;
    if (*limit == 0U ? memory.rss_bytes != 0U
                     : memory.rss_bytes >= *limit) {
      critical = true;
    } else if (*limit != 0U &&
               memory.rss_bytes > *limit - *limit / 4U) {
      constrained = true;
    }
  }
  if (critical) return HostMemoryPressureLevel::Critical;
  if (constrained) return HostMemoryPressureLevel::Constrained;
  return observed ? HostMemoryPressureLevel::Normal
                  : HostMemoryPressureLevel::Unknown;
}

[[nodiscard]] bool host_memory_is_pressured(
    const HostMemoryBudgetSnapshot &memory) {
  const auto level = host_memory_pressure_level(memory);
  return level == HostMemoryPressureLevel::Constrained ||
         level == HostMemoryPressureLevel::Critical;
}

[[nodiscard]] std::string_view host_memory_pressure_name(
    const HostMemoryBudgetSnapshot &memory) {
  switch (host_memory_pressure_level(memory)) {
  case HostMemoryPressureLevel::Unknown:
    return "unknown";
  case HostMemoryPressureLevel::Normal:
    return "normal";
  case HostMemoryPressureLevel::Constrained:
    return "constrained";
  case HostMemoryPressureLevel::Critical:
    return "critical";
  }
  return "unknown";
}

class GuestTickClock {
public:
  explicit GuestTickClock(std::uint32_t ticks_per_second)
      : ticks_per_second_{ticks_per_second} {
    if (ticks_per_second_ == 0) {
      throw std::invalid_argument{"guest tick rate must be non-zero"};
    }
  }

  [[nodiscard]] std::uint64_t absolute_time_units(
      std::uint64_t ticks) {
    constexpr auto units_per_second =
        darwin::mach::thread_policy::absolute_time_units_per_second;
    const auto whole_seconds = ticks / ticks_per_second_;
    if (whole_seconds >
        std::numeric_limits<std::uint64_t>::max() / units_per_second) {
      throw std::overflow_error{"guest time conversion overflow"};
    }
    const auto fractional_ticks = ticks % ticks_per_second_;
    const auto scaled_fraction =
        fractional_ticks * units_per_second + remainder_;
    remainder_ = scaled_fraction % ticks_per_second_;
    return whole_seconds * units_per_second +
           scaled_fraction / ticks_per_second_;
  }

private:
  std::uint64_t ticks_per_second_{};
  std::uint64_t remainder_{};
};

[[nodiscard]] std::uint64_t duration_to_guest_ticks(
    std::uint64_t value, std::uint64_t units_per_second,
    std::uint32_t guest_ticks_per_second) {
  if (units_per_second == 0 || guest_ticks_per_second == 0) {
    throw std::invalid_argument{"time conversion rate must be non-zero"};
  }
  const auto whole_seconds = value / units_per_second;
  const auto fractional_units = value % units_per_second;
  if (whole_seconds >
      std::numeric_limits<std::uint64_t>::max() / guest_ticks_per_second ||
      fractional_units >
          std::numeric_limits<std::uint64_t>::max() /
              guest_ticks_per_second) {
    throw std::overflow_error{"guest tick conversion overflow"};
  }
  const auto whole_ticks = whole_seconds * guest_ticks_per_second;
  const auto fractional_ticks =
      fractional_units * guest_ticks_per_second / units_per_second;
  if (fractional_ticks >
      std::numeric_limits<std::uint64_t>::max() - whole_ticks) {
    throw std::overflow_error{"guest tick conversion overflow"};
  }
  return whole_ticks + fractional_ticks;
}

struct PendingExec {
  std::size_t processor{};
  std::string path;
  std::vector<std::string> arguments;
  std::vector<std::string> environment;
};

class JitCodeCacheGovernor;

enum class JitCodeCacheClass : std::uint8_t {
  BootCritical,
  Foreground,
  Background,
};

class JitCodeCacheReservation {
public:
  JitCodeCacheReservation(JitCodeCacheGovernor &governor,
                          std::size_t shared_slab_bytes,
                          std::size_t reserved_bytes,
                          std::size_t maximum_bytes,
                          JitCodeCacheClass cache_class, bool emergency)
      : governor_{&governor},
        shared_slab_bytes_{shared_slab_bytes},
        reserved_bytes_{reserved_bytes},
        maximum_bytes_{maximum_bytes},
        cache_class_{cache_class},
        emergency_{emergency} {}
  ~JitCodeCacheReservation();

  JitCodeCacheReservation(const JitCodeCacheReservation &) = delete;
  JitCodeCacheReservation &operator=(const JitCodeCacheReservation &) =
      delete;

  [[nodiscard]] std::size_t shared_slab_bytes() const noexcept {
    return shared_slab_bytes_;
  }

private:
  friend class JitCodeCacheGovernor;

  JitCodeCacheGovernor *governor_{};
  std::size_t shared_slab_bytes_{};
  std::size_t reserved_bytes_{};
  std::size_t maximum_bytes_{};
  JitCodeCacheClass cache_class_{JitCodeCacheClass::Background};
  std::uint64_t actual_bytes_{};
  bool emergency_{};
};

class JitCodeCacheGovernor {
public:
  JitCodeCacheGovernor(std::size_t shared_slab_cap,
                       std::size_t total_budget)
      : shared_slab_cap_{shared_slab_cap},
        total_budget_{std::max({total_budget, emergency_budget_bytes,
                                minimum_shared_slab_bytes})},
        normal_budget_{total_budget_ - emergency_budget_bytes} {}

  JitCodeCacheGovernor(const JitCodeCacheGovernor &) = delete;
  JitCodeCacheGovernor &operator=(const JitCodeCacheGovernor &) = delete;

  [[nodiscard]] std::shared_ptr<JitCodeCacheReservation> reserve(
      std::size_t processor_count,
      JitCodeCacheClass cache_class = JitCodeCacheClass::Background) {
    if (processor_count == 0U) return {};
    std::lock_guard lock{mutex_};
    const auto class_cap = shared_slab_cap_for_locked(cache_class);
    const auto normal_available = normal_reserved_bytes_ < normal_budget_
                                      ? normal_budget_ - normal_reserved_bytes_
                                      : 0U;
    if (class_cap >= minimum_shared_slab_bytes &&
        normal_available >= minimum_shared_slab_bytes) {
      const auto shared_slab = std::min(class_cap, normal_available);
      const auto reserved = shared_slab;
      auto reservation = std::make_shared<JitCodeCacheReservation>(
          *this, shared_slab, reserved, class_cap, cache_class, false);
      normal_reserved_bytes_ += reserved;
      return reservation;
    }
    // Keep a bounded minimum-cache pool so normal firmware process fan-out
    // does not turn a cache admission failure into an unbounded overcommit.
    // Once this pool is exhausted, the caller receives the kernel's EAGAIN
    // process-creation result.
    const auto emergency_available =
        emergency_reserved_bytes_ < emergency_budget_bytes
            ? emergency_budget_bytes - emergency_reserved_bytes_
            : 0U;
    if (class_cap < minimum_shared_slab_bytes ||
        emergency_available < minimum_shared_slab_bytes) {
      return {};
    }
    const auto shared_slab = minimum_shared_slab_bytes;
    const auto reserved = shared_slab;
    auto reservation = std::make_shared<JitCodeCacheReservation>(
        *this, shared_slab, reserved, class_cap, cache_class, true);
    emergency_reserved_bytes_ += reserved;
    return reservation;
  }

  [[nodiscard]] std::size_t shared_slab_cap_for(
      JitCodeCacheClass cache_class) const noexcept {
    std::lock_guard lock{mutex_};
    return shared_slab_cap_for_locked(cache_class);
  }

  void set_pressure_limited(bool limited) noexcept {
    std::lock_guard lock{mutex_};
    pressure_limited_ = limited;
  }

  [[nodiscard]] bool pressure_limited() const noexcept {
    std::lock_guard lock{mutex_};
    return pressure_limited_;
  }

private:
  [[nodiscard]] std::size_t shared_slab_cap_for_locked(
      JitCodeCacheClass cache_class) const noexcept {
    std::size_t cap{};
    switch (cache_class) {
    case JitCodeCacheClass::BootCritical:
      cap = shared_slab_cap_;
      break;
    case JitCodeCacheClass::Foreground:
      cap = std::max(minimum_shared_slab_bytes,
                     shared_slab_cap_ * 3U / 4U);
      break;
    case JitCodeCacheClass::Background:
      cap = std::max(minimum_shared_slab_bytes,
                     shared_slab_cap_ / 2U);
      break;
    default:
      cap = minimum_shared_slab_bytes;
      break;
    }
    if (pressure_limited_ && cache_class != JitCodeCacheClass::BootCritical) {
      cap = std::max(minimum_shared_slab_bytes, cap / 2U);
    }
    return cap;
  }

public:
  [[nodiscard]] std::optional<std::size_t> reclassify(
      JitCodeCacheReservation &reservation, JitCodeCacheClass cache_class,
      bool slab_can_resize) noexcept {
    std::lock_guard lock{mutex_};
    const auto class_cap = shared_slab_cap_for_locked(cache_class);
    const auto requested_maximum = class_cap;
    if (!slab_can_resize) {
      // A live Dynarmic slab cannot be shrunk or grown by changing this
      // bookkeeping object.  Retain its full existing allowance so another
      // runtime cannot consume bytes that the live mapping may still use.
      reservation.cache_class_ = cache_class;
      reservation.maximum_bytes_ = reservation.shared_slab_bytes_;
      return reservation.shared_slab_bytes_;
    }
    if (reservation.emergency_) {
      reservation.cache_class_ = cache_class;
      reservation.maximum_bytes_ = requested_maximum;
      return reservation.shared_slab_bytes_;
    }

    const auto current_maximum = reservation.shared_slab_bytes_;
    const auto actual_floor = reservation.actual_bytes_ >
                                      std::numeric_limits<std::size_t>::max()
                                  ? std::numeric_limits<std::size_t>::max()
                                  : static_cast<std::size_t>(
                                        reservation.actual_bytes_);
    const auto effective_maximum = std::max(requested_maximum, actual_floor);
    if (effective_maximum < current_maximum) {
      const auto release_bytes = current_maximum - effective_maximum;
      const auto released =
          std::min(release_bytes, reservation.reserved_bytes_);
      reservation.reserved_bytes_ -= released;
      normal_reserved_bytes_ = released > normal_reserved_bytes_
                                   ? 0U
                                   : normal_reserved_bytes_ - released;
      reservation.shared_slab_bytes_ =
          std::max(minimum_shared_slab_bytes, effective_maximum);
    } else if (effective_maximum > current_maximum) {
      const auto growth = effective_maximum - current_maximum;
      const auto available = normal_reserved_bytes_ < normal_budget_
                                 ? normal_budget_ - normal_reserved_bytes_
                                 : 0U;
      if (growth <= available) {
        reservation.reserved_bytes_ += growth;
        normal_reserved_bytes_ += growth;
        reservation.shared_slab_bytes_ = std::max(class_cap, effective_maximum);
      }
    }
    reservation.cache_class_ = cache_class;
    reservation.maximum_bytes_ =
        reservation.shared_slab_bytes_;
    return reservation.shared_slab_bytes_;
  }

  // A reservation is a bounded growth allowance, not a claim that every byte
  // has already been emitted. Reconcile it with Dynarmic's measured
  // CodeCacheUsed and current host pressure while the guest is idle.
  [[nodiscard]] bool refresh_actual(
      JitCodeCacheReservation &reservation, std::uint64_t actual_bytes,
      const HostMemoryBudgetSnapshot &memory) noexcept {
    std::lock_guard lock{mutex_};
    static_cast<void>(memory);
    const auto previous_actual = reservation.actual_bytes_;
    reservation.actual_bytes_ = actual_bytes;
    // The slab was created with shared_slab_bytes_ as its maximum.  Keep that
    // reservation until the Runtime is destroyed; releasing an apparent
    // unused tail here would let another runtime reserve memory that this
    // still-live mapping can legally grow into.
    if (actual_bytes > previous_actual) {
      const auto delta = actual_bytes - previous_actual;
      actual_bytes_total_ =
          delta > std::numeric_limits<std::size_t>::max() - actual_bytes_total_
              ? std::numeric_limits<std::size_t>::max()
              : actual_bytes_total_ + static_cast<std::size_t>(delta);
    } else {
      const auto delta = previous_actual - actual_bytes;
      actual_bytes_total_ =
          delta > actual_bytes_total_ ? 0U : actual_bytes_total_ - delta;
    }
    return previous_actual != actual_bytes;
  }

  void release(std::size_t reserved_bytes, bool emergency,
               std::uint64_t actual_bytes) noexcept {
    std::lock_guard lock{mutex_};
    auto &reserved = emergency ? emergency_reserved_bytes_
                               : normal_reserved_bytes_;
    reserved = reserved_bytes > reserved ? 0U : reserved - reserved_bytes;
    actual_bytes_total_ = actual_bytes > actual_bytes_total_
                              ? 0U
                              : actual_bytes_total_ -
                                    static_cast<std::size_t>(actual_bytes);
  }

  [[nodiscard]] std::size_t total_budget() const noexcept {
    return total_budget_;
  }

  [[nodiscard]] std::size_t total_reserved() const noexcept {
    std::lock_guard lock{mutex_};
    return normal_reserved_bytes_ + emergency_reserved_bytes_;
  }

  [[nodiscard]] std::size_t total_actual() const noexcept {
    std::lock_guard lock{mutex_};
    return actual_bytes_total_;
  }

  [[nodiscard]] std::size_t shared_slab_cap(
      JitCodeCacheClass cache_class) const noexcept {
    return shared_slab_cap_for(cache_class);
  }

  [[nodiscard]] std::size_t emergency_budget() const noexcept {
    return emergency_budget_bytes;
  }

private:
  static constexpr std::size_t minimum_shared_slab_bytes =
      jit_minimum_shared_slab_bytes;
  static constexpr std::size_t emergency_budget_bytes =
      jit_emergency_budget_bytes;

  friend class JitCodeCacheReservation;

  const std::size_t shared_slab_cap_;
  const std::size_t total_budget_;
  const std::size_t normal_budget_;
  mutable std::mutex mutex_;
  std::size_t normal_reserved_bytes_{};
  std::size_t emergency_reserved_bytes_{};
  std::size_t actual_bytes_total_{};
  bool pressure_limited_{};
};

JitCodeCacheReservation::~JitCodeCacheReservation() {
  if (governor_ != nullptr) {
    governor_->release(reserved_bytes_, emergency_, actual_bytes_);
  }
}

struct RuntimeWorkEpoch {
  [[nodiscard]] std::uint64_t current() const noexcept {
    return epoch_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t begin_transition() noexcept {
    cancelled_.store(true, std::memory_order_release);
    auto expected = epoch_.load(std::memory_order_relaxed);
    for (;;) {
      if (expected == std::numeric_limits<std::uint64_t>::max()) return expected;
      if (epoch_.compare_exchange_weak(
              expected, expected + 1U, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        return expected + 1U;
      }
    }
  }

  void activate(std::uint64_t expected_epoch) noexcept {
    if (epoch_.load(std::memory_order_acquire) == expected_epoch)
      cancelled_.store(false, std::memory_order_release);
  }

  [[nodiscard]] bool stop_requested(
      std::uint64_t expected_epoch) const noexcept {
    return cancelled_.load(std::memory_order_acquire) ||
           epoch_.load(std::memory_order_acquire) != expected_epoch;
  }

private:
  std::atomic<std::uint64_t> epoch_{1};
  std::atomic<bool> cancelled_{};
};

struct Runtime {
  // Keep the reservation before the native runtime fields so its destructor
  // releases the budget only after Dynarmic's code cache has been destroyed.
  std::shared_ptr<JitCodeCacheReservation> jit_cache_reservation;
  JitCodeCacheClass jit_cache_class{JitCodeCacheClass::Background};
  std::unique_ptr<AddressSpace> memory;
  std::unique_ptr<CpuCluster> cpus;
  std::unique_ptr<CompatibilityKernel> kernel;
  std::vector<bool> allocated;
  std::optional<PendingExec> pending_exec;
  std::shared_ptr<HostWorkToken> precompile_task;
  JitPrecompilePhase precompile_phase{JitPrecompilePhase::Remaining};
  RuntimeWorkEpoch work_epoch;
  std::optional<std::chrono::steady_clock::time_point>
      execution_reclaim_after;
  bool fresh_spawn_address_space{};

  [[nodiscard]] std::uint64_t begin_image_transition(
      HostResourceController &host_resources) {
    const auto next_epoch = work_epoch.begin_transition();
    const auto task = precompile_task;
    if (task) task->cancel();
    host_resources.wake();
    if (cpus) cpus->quiesce_precompilation();
    if (task) task->wait_finished();
    return next_epoch;
  }

  void activate_image_epoch(std::uint64_t epoch) noexcept {
    work_epoch.activate(epoch);
  }

  [[nodiscard]] bool precompile_stop_requested(
      std::uint64_t expected_epoch) const noexcept {
    return work_epoch.stop_requested(expected_epoch);
  }

  ~Runtime() {
    PerformanceLatencyScope latency{PerfLatencyKind::RuntimeDestructor};
    static_cast<void>(work_epoch.begin_transition());
    if (precompile_task) precompile_task->cancel();
    if (cpus) cpus->quiesce_precompilation();
    pending_exec.reset();
    std::vector<bool>{}.swap(allocated);
    kernel.reset();
    cpus.reset();
    memory.reset();
  }
};

class RuntimeIndex {
public:
  RuntimeIndex() = default;
  RuntimeIndex(const RuntimeIndex &) = delete;
  RuntimeIndex &operator=(const RuntimeIndex &) = delete;

  void insert(Runtime &runtime) {
    const auto pid = runtime.kernel->process().pid;
    const auto [entry, inserted] = runtimes_.emplace(pid, &runtime);
    if (!inserted && entry->second != &runtime) {
      throw std::logic_error{"duplicate Runtime process id"};
    }
  }

  void erase(const Runtime &runtime) {
    const auto pid = runtime.kernel->process().pid;
    const auto entry = runtimes_.find(pid);
    if (entry != runtimes_.end() && entry->second == &runtime)
      runtimes_.erase(entry);
  }

  [[nodiscard]] Runtime *find(std::uint32_t pid) const {
    const auto entry = runtimes_.find(pid);
    return entry == runtimes_.end() ? nullptr : entry->second;
  }

private:
  std::unordered_map<std::uint32_t, Runtime *> runtimes_;
};

class RuntimeReaper {
public:
  RuntimeReaper() : worker_{[this] { worker_loop(); }} {}
  RuntimeReaper(const RuntimeReaper &) = delete;
  RuntimeReaper &operator=(const RuntimeReaper &) = delete;

  ~RuntimeReaper() { finish(); }

  void retire(std::unique_ptr<Runtime> runtime) {
    retire_resource(std::move(runtime));
  }

  void retire_execution_resources(
      std::shared_ptr<CpuExecutionPool> resources) {
    retire_resource(std::move(resources));
  }

  void finish() {
    {
      std::lock_guard lock{mutex_};
      if (joined_)
        return;
      stopping_ = true;
    }
    work_available_.notify_one();
    {
      std::unique_lock lock{mutex_};
      idle_.wait(lock, [this] { return pending_.empty() && !active_; });
    }
    if (worker_.joinable())
      worker_.join();
    std::lock_guard lock{mutex_};
    joined_ = true;
  }

private:
  using RetiredResource =
      std::variant<std::monostate, std::unique_ptr<Runtime>,
                   std::shared_ptr<CpuExecutionPool>>;

  template <typename Resource>
  void retire_resource(Resource resource) {
    if (!resource)
      return;
    {
      std::lock_guard lock{mutex_};
      if (stopping_)
        throw std::logic_error{"cannot retire a Runtime after reaper stop"};
      pending_.push_back(std::move(resource));
    }
    work_available_.notify_one();
  }

  void worker_loop() {
    std::unique_lock lock{mutex_};
    for (;;) {
      work_available_.wait(
          lock, [this] { return stopping_ || !pending_.empty(); });
      if (pending_.empty()) {
        if (stopping_)
          break;
        continue;
      }
      auto resource = std::move(pending_.front());
      pending_.pop_front();
      active_ = true;
      lock.unlock();
      resource = std::monostate{};
      lock.lock();
      active_ = false;
      idle_.notify_all();
    }
    idle_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::deque<RetiredResource> pending_;
  bool active_{};
  bool stopping_{};
  bool joined_{};
  std::thread worker_;
};

struct PreparedGuestSlice {
  XnuScheduledSlice scheduled;
  Runtime *runtime{};
  std::size_t thread_index{};
  Cpu *cpu{};
  std::uint64_t tick_budget{};
  std::chrono::nanoseconds host_slice_budget{};
  bool single_step{};
  bool deferred_svc{};
  CpuRunResult result;
  std::exception_ptr error;
};

class GuestSliceWorkerPool {
public:
  explicit GuestSliceWorkerPool(std::size_t worker_count) {
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  GuestSliceWorkerPool(const GuestSliceWorkerPool &) = delete;
  GuestSliceWorkerPool &operator=(const GuestSliceWorkerPool &) = delete;

  ~GuestSliceWorkerPool() {
    {
      std::lock_guard lock{mutex_};
      stopping_ = true;
    }
    work_available_.notify_all();
    for (auto &worker : workers_)
      worker.join();
  }

  void run(std::vector<PreparedGuestSlice> &slices) {
    if (slices.empty())
      return;
    {
      std::lock_guard lock{mutex_};
      if (remaining_ != 0)
        throw std::logic_error{"guest slice worker batch overlaps"};
      slices_ = slices.data();
      slice_count_ = slices.size();
      next_slice_ = 0;
      remaining_ = slices.size();
      if (++generation_ == 0)
        ++generation_;
    }
    work_available_.notify_all();
    std::unique_lock lock{mutex_};
    batch_complete_.wait(lock, [this] { return remaining_ == 0; });
    slices_ = nullptr;
    slice_count_ = 0;
  }

  static void execute(PreparedGuestSlice &prepared) {
    try {
      prepared.result =
          prepared.single_step
              ? prepared.cpu->step(prepared.scheduled.processor)
              : prepared.cpu->run_cooperatively(
                    prepared.tick_budget, prepared.host_slice_budget,
                    prepared.scheduled.processor);
    } catch (...) {
      prepared.error = std::current_exception();
    }
  }

private:
  void worker_loop() {
    std::uint64_t observed_generation{};
    std::unique_lock lock{mutex_};
    while (true) {
      work_available_.wait(lock, [&] {
        return stopping_ || generation_ != observed_generation;
      });
      if (stopping_)
        return;
      observed_generation = generation_;
      while (next_slice_ < slice_count_) {
        auto *slice = slices_ + next_slice_++;
        lock.unlock();
        execute(*slice);
        lock.lock();
        if (--remaining_ == 0)
          batch_complete_.notify_one();
      }
    }
  }

  std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable batch_complete_;
  std::vector<std::thread> workers_;
  PreparedGuestSlice *slices_{};
  std::size_t slice_count_{};
  std::size_t next_slice_{};
  std::size_t remaining_{};
  std::uint64_t generation_{};
  bool stopping_{};
};

class BootGdbTarget final : public GdbTarget {
public:
  explicit BootGdbTarget(std::vector<std::unique_ptr<Runtime>> &runtimes)
      : runtimes_{runtimes} {}

  [[nodiscard]] std::vector<GdbThreadId> threads() const override {
    std::vector<GdbThreadId> result;
    for (const auto &runtime : runtimes_) {
      for (std::size_t processor = 0; processor < runtime->allocated.size();
           ++processor) {
        if (runtime->allocated[processor]) {
          result.push_back(
              GdbThreadId{runtime->kernel->process().pid,
                          static_cast<std::uint32_t>(processor + 1U)});
        }
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<GdbThreadId> current_thread() const override {
    return current_thread_;
  }

  void set_current_thread(GdbThreadId thread) { current_thread_ = thread; }

  [[nodiscard]] std::optional<std::string>
  thread_extra_info(GdbThreadId thread) const override {
    const auto selected = find_thread(thread);
    if (!selected)
      return std::nullopt;
    return "pid " + std::to_string(thread.process) + " thread " +
           std::to_string(thread.thread) +
           " wait=" + selected->first->kernel->wait_reason(selected->second);
  }

  [[nodiscard]] std::optional<GdbArmRegisters>
  read_registers(GdbThreadId thread) const override {
    const auto selected = find_thread(thread);
    if (!selected)
      return std::nullopt;
    GdbArmRegisters result{};
    const auto &cpu = selected->first->cpus->cpu(selected->second);
    std::copy(cpu.registers().begin(), cpu.registers().end(), result.begin());
    result[gdb_arm_cpsr_register] = cpu.cpsr();
    return result;
  }

  bool write_registers(GdbThreadId thread,
                       const GdbArmRegisters &registers) override {
    const auto selected = find_thread(thread);
    if (!selected)
      return false;
    auto &cpu = selected->first->cpus->cpu(selected->second);
    std::copy_n(registers.begin(), gdb_arm_general_register_count,
                cpu.registers().begin());
    cpu.set_cpsr(registers[gdb_arm_cpsr_register]);
    return true;
  }

  [[nodiscard]] std::optional<std::vector<std::byte>>
  read_memory(GdbThreadId thread, std::uint32_t address,
              std::size_t size) const override {
    const auto selected = find_thread(thread);
    return selected ? selected->first->memory->read_bytes(address, size)
                    : std::nullopt;
  }

  bool write_memory(GdbThreadId thread, std::uint32_t address,
                    std::span<const std::byte> bytes) override {
    const auto selected = find_thread(thread);
    if (!selected || !selected->first->memory->copy_in(address, bytes))
      return false;
    clear_process_cache(*selected->first);
    return true;
  }

  bool insert_software_breakpoint(GdbThreadId thread, std::uint32_t address,
                                  std::size_t kind) override {
    const auto selected = find_thread(thread);
    if (!selected ||
        (kind != arm_thumb_breakpoint_size && kind != arm_breakpoint_size) ||
        (address & static_cast<std::uint32_t>(kind - 1U)) != 0) {
      return false;
    }
    const auto key = std::pair{thread.process, address};
    if (const auto existing = breakpoints_.find(key);
        existing != breakpoints_.end()) {
      return existing->second.kind == kind;
    }
    const auto original = selected->first->memory->read_bytes(address, kind);
    if (!original)
      return false;
    static constexpr std::array<std::byte, arm_thumb_breakpoint_size>
        thumb_breakpoint{std::byte{0x00}, std::byte{0xbe}};
    static constexpr std::array<std::byte, arm_breakpoint_size> arm_breakpoint{
        std::byte{0x70}, std::byte{0x00}, std::byte{0x20}, std::byte{0xe1}};
    const auto instruction = kind == arm_thumb_breakpoint_size
                                 ? std::span<const std::byte>{thumb_breakpoint}
                                 : std::span<const std::byte>{arm_breakpoint};
    if (!selected->first->memory->copy_in(address, instruction))
      return false;
    breakpoints_.emplace(key, BreakpointRecord{kind, std::move(*original)});
    clear_process_cache(*selected->first);
    return true;
  }

  bool remove_software_breakpoint(GdbThreadId thread, std::uint32_t address,
                                  std::size_t kind) override {
    const auto selected = find_thread(thread);
    const auto breakpoint = breakpoints_.find({thread.process, address});
    if (!selected || breakpoint == breakpoints_.end() ||
        breakpoint->second.kind != kind ||
        !selected->first->memory->copy_in(address,
                                          breakpoint->second.original)) {
      return false;
    }
    breakpoints_.erase(breakpoint);
    clear_process_cache(*selected->first);
    return true;
  }

  void prepare_fork_child(std::uint32_t parent_pid,
                          AddressSpace &child_memory) const {
    for (const auto &[key, breakpoint] : breakpoints_) {
      if (key.first == parent_pid) {
        static_cast<void>(
            child_memory.copy_in(key.second, breakpoint.original));
      }
    }
  }

  void notify_exec(std::uint32_t process) {
    std::erase_if(breakpoints_, [process](const auto &item) {
      return item.first.first == process;
    });
  }

  void remove_all_breakpoints() {
    for (const auto &[key, breakpoint] : breakpoints_) {
      for (const auto &runtime : runtimes_) {
        if (runtime->kernel->process().pid == key.first) {
          static_cast<void>(
              runtime->memory->copy_in(key.second, breakpoint.original));
          clear_process_cache(*runtime);
          break;
        }
      }
    }
    breakpoints_.clear();
  }

private:
  struct BreakpointRecord {
    std::size_t kind{};
    std::vector<std::byte> original;
  };

  [[nodiscard]] std::optional<std::pair<Runtime *, std::size_t>>
  find_thread(GdbThreadId thread) const {
    if (thread.thread == 0)
      return std::nullopt;
    const auto processor = static_cast<std::size_t>(thread.thread - 1U);
    for (const auto &runtime : runtimes_) {
      if (runtime->kernel->process().pid == thread.process &&
          processor < runtime->allocated.size() &&
          runtime->allocated[processor]) {
        return std::pair{runtime.get(), processor};
      }
    }
    return std::nullopt;
  }

  static void clear_process_cache(Runtime &runtime) {
    for (std::size_t processor = 0; processor < runtime.cpus->size();
         ++processor) {
      runtime.cpus->cpu(processor).clear_cache();
    }
  }

  std::vector<std::unique_ptr<Runtime>> &runtimes_;
  std::optional<GdbThreadId> current_thread_;
  std::map<std::pair<std::uint32_t, std::uint32_t>, BreakpointRecord>
      breakpoints_;
};

std::string usage() {
  return "Usage:\n"
         "  ilemu profile [--device iPhone1,1|iPhone1,2|iPhone2,1] [--output FILE]\n"
         "  ilemu inspect --rootfs DIR [--binary /sbin/launchd] "
         "[--device PROFILE] [--symbols SUBSTRING] [--output FILE]\n"
         "  ilemu catalog --rootfs DIR [--device PROFILE] [--manifest FILE] "
         "[--host-cache DIR] "
         "[--output FILE]\n"
         "  ilemu firmware prepare --rootfs DIR [--device PROFILE] "
         "[--manifest FILE] [--host-cache DIR] [--prepare-force] "
         "[--prepare-file-blocks N] [--prepare-image-blocks N] "
         "[--prepare-firmware-blocks N] [--prepare-file-ms N] "
         "[--prepare-image-ms N] [--prepare-firmware-ms N] "
         "[--jit-artifact-memory-mib 1..4096] "
         "[--jit-artifact-disk-mib 0..4096] [--output FILE]\n"
         "  ilemu disasm --rootfs DIR --binary PATH "
         "(--symbol NAME | --address ADDR) [--device PROFILE] [--count N] [--thumb]\n"
         "  ilemu boot --rootfs DIR [--device iPhone1,1|iPhone1,2|iPhone2,1] "
         "[--binary /sbin/launchd] [--ticks N] "
         "[--cores N] [--jit-cache-mib 8..128] "
         "[--jit-cache-budget-mib 256..4096] "
         "[--watch-address ADDR] [--gdb PORT] "
         "[--display headless|sdl] [--network isolated|loopback|host] "
         "[--gles-backend auto|software|vulkan] [--gpu] "
         "[--host-cache DIR] [--catalog FILE] "
         "[--jit-artifact-disk-mib 0..4096] "
         "[--jit-artifact-memory-mib 1..4096] "
         "[--display-size WIDTHxHEIGHT] "
         "[--activation activated|unactivated|preserve] "
         "[--frame-output FILE] [--touch-replay FILE] [--control-stdin] "
         "[--baseband-input FILE] [--baseband-output FILE] "
         "[--perf-summary] [--perf-frame-content] [--perf-cpu-phases] "
         "[--output FILE]\n"
         "  ilemu smoke [--cores N] [--jit-cache-mib 8..128] "
         "[--perf-summary] [--output FILE]\n"
         "  ilemu benchmark arm [--iterations N] "
         "[--jit-cache-mib 8..128] [--perf-summary] "
         "[--output FILE]\n";
}

std::optional<std::string> option(const std::vector<std::string> &args,
                                  std::string_view name) {
  const auto inline_prefix = std::string{name} + "=";
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == name) {
      if (i + 1 >= args.size()) {
        throw std::runtime_error{"missing value for " + std::string{name}};
      }
      return args[i + 1];
    }
    if (args[i].starts_with(inline_prefix)) {
      const auto value = args[i].substr(inline_prefix.size());
      if (value.empty()) {
        throw std::runtime_error{"missing value for " + std::string{name}};
      }
      return value;
    }
  }
  return std::nullopt;
}

std::filesystem::path host_cache_directory(
    const std::vector<std::string> &args,
    const std::filesystem::path &rootfs) {
  if (const auto configured = option(args, "--host-cache")) {
    return std::filesystem::path{*configured};
  }
  const auto normalized = rootfs.lexically_normal();
  auto rootfs_name = normalized.filename();
  if (rootfs_name.empty() || rootfs_name == "." ||
      rootfs_name == normalized.root_name()) {
    rootfs_name = "rootfs";
  }
  return normalized.parent_path() / ".ilegacysim-cache" / rootfs_name;
}

std::filesystem::path nearest_existing_filesystem_path(
    const std::filesystem::path &path) {
  std::error_code error;
  auto candidate = std::filesystem::absolute(path, error);
  if (error || candidate.empty()) candidate = path;
  for (;;) {
    const auto status = std::filesystem::status(candidate, error);
    if (!error && status.type() != std::filesystem::file_type::not_found) {
      return candidate;
    }
    const auto parent = candidate.parent_path();
    if (parent.empty() || parent == candidate) return {};
    candidate = parent;
    error.clear();
  }
}

bool flag(const std::vector<std::string> &args, std::string_view name) {
  return std::find(args.begin(), args.end(), name) != args.end();
}

std::size_t jit_code_cache_size(const std::vector<std::string> &args) {
  const auto value = option(args, "--jit-cache-mib").value_or("64");
  std::size_t consumed{};
  const auto mebibytes = std::stoull(value, &consumed, 10);
  if (consumed != value.size() || mebibytes < 8U || mebibytes > 128U) {
    throw std::runtime_error{
        "--jit-cache-mib must be in the range 8..128"};
  }
  return static_cast<std::size_t>(mebibytes) * 1024U * 1024U;
}

std::uintmax_t parse_mib_value(std::string_view value, std::string_view name,
                               std::uintmax_t minimum,
                               std::uintmax_t maximum) {
  std::size_t consumed{};
  const auto mebibytes = std::stoull(std::string{value}, &consumed, 10);
  if (consumed != value.size() || mebibytes < minimum ||
      mebibytes > maximum ||
      mebibytes > std::numeric_limits<std::uintmax_t>::max() /
                        (1024U * 1024U)) {
    throw std::runtime_error{std::string{name} + " must be in the range " +
                             std::to_string(minimum) + ".." +
                             std::to_string(maximum) + " MiB"};
  }
  return static_cast<std::uintmax_t>(mebibytes) * 1024U * 1024U;
}

std::size_t parse_prepare_count(const std::vector<std::string> &args,
                                std::string_view name, std::size_t fallback,
                                std::size_t maximum) {
  const auto value = option(args, name).value_or(std::to_string(fallback));
  std::size_t consumed{};
  const auto parsed = std::stoull(value, &consumed, 10);
  if (consumed != value.size() || parsed == 0U || parsed > maximum) {
    throw std::runtime_error{std::string{name} + " must be in the range 1.." +
                             std::to_string(maximum)};
  }
  return static_cast<std::size_t>(parsed);
}

std::chrono::milliseconds parse_prepare_time(
    const std::vector<std::string> &args, std::string_view name,
    std::size_t fallback, std::size_t maximum) {
  return std::chrono::milliseconds{
      parse_prepare_count(args, name, fallback, maximum)};
}

struct JitCodeCacheBudget {
  std::size_t total_bytes{};
  HostMemoryBudgetSnapshot memory;
  bool explicit_override{};
};

[[nodiscard]] JitCodeCacheBudget jit_code_cache_budget(
    const std::vector<std::string>& args) {
  const auto memory = host_memory_budget_snapshot();
  const auto minimum_total =
      jit_emergency_budget_bytes + jit_minimum_shared_slab_bytes;
  if (const auto configured = option(args, "--jit-cache-budget-mib")) {
    return JitCodeCacheBudget{
        static_cast<std::size_t>(parse_mib_value(
            *configured, "--jit-cache-budget-mib", 256U, 4096U)),
        memory,
        true,
    };
  }

  const auto effective_limit_value = effective_host_memory_limit(memory);
  const auto effective_limit = effective_limit_value.value_or(
      static_cast<std::uint64_t>(jit_maximum_adaptive_budget_bytes));
  bool headroom_known = memory.available_known;
  auto headroom = memory.available_bytes;
  if (memory.cgroup_limit_known && memory.cgroup_current_known) {
    const auto cgroup_headroom =
        memory.cgroup_current_bytes >= memory.cgroup_limit_bytes
            ? std::uint64_t{0}
            : memory.cgroup_limit_bytes - memory.cgroup_current_bytes;
    if (!headroom_known) {
      headroom = cgroup_headroom;
      headroom_known = true;
    } else {
      headroom = std::min(headroom, cgroup_headroom);
    }
  }
  if (effective_limit_value && memory.rss_known) {
    const auto rss_headroom = memory.rss_bytes >= *effective_limit_value
                                  ? std::uint64_t{0}
                                  : *effective_limit_value - memory.rss_bytes;
    if (!headroom_known) {
      headroom = rss_headroom;
      headroom_known = true;
    } else {
      headroom = std::min(headroom, rss_headroom);
    }
  }
  // Keep code-cache reservations below both a fraction of total capacity and
  // a fraction of current headroom. The hard ceiling remains a safety bound;
  // unlike the old fixed floor, low-memory/cgroup pressure can lower it.
  const auto capacity_target = effective_limit / 8U;
  const auto pressure_target = headroom / 2U;
  auto target = headroom_known ? std::min(capacity_target, pressure_target)
                               : capacity_target;
  // A known zero headroom is real pressure, not missing host information.
  // Keep only the minimum safety pool in that case. When host information is
  // unavailable, use the bounded capacity target; never invent a 640 MiB
  // reservation from an unknown zero.
  if (target == 0U) target = minimum_total;
  target = std::clamp<std::uint64_t>(
      target, static_cast<std::uint64_t>(minimum_total),
      static_cast<std::uint64_t>(jit_maximum_adaptive_budget_bytes));
  return JitCodeCacheBudget{static_cast<std::size_t>(target), memory, false};
}

std::size_t jit_artifact_memory_limit(
    const std::vector<std::string> &args) {
  const auto value = option(args, "--jit-artifact-memory-mib").value_or("64");
  return static_cast<std::size_t>(parse_mib_value(
      value, "--jit-artifact-memory-mib", 1U, 4096U));
}

GlesBackend parse_gles_backend(const std::vector<std::string> &args) {
  const auto configured = option(args, "--gles-backend");
  auto backend = GlesBackend::Auto;
  if (configured) {
    if (*configured == "software") {
      backend = GlesBackend::Software;
    } else if (*configured == "vulkan") {
      backend = GlesBackend::Vulkan;
    } else if (*configured != "auto") {
      throw std::runtime_error{
          "--gles-backend must be auto, software, or vulkan"};
    }
  }
  if (flag(args, "--gpu")) {
    if (configured && backend == GlesBackend::Software) {
      throw std::runtime_error{
          "--gpu conflicts with --gles-backend=software"};
    }
    backend = GlesBackend::Vulkan;
  }
  return backend;
}

DisplayGeometry parse_display_geometry(std::string_view value) {
  const auto separator = value.find_first_of("xX");
  if (separator == std::string_view::npos || separator == 0U ||
      separator + 1U >= value.size()) {
    throw std::runtime_error{"--display-size must use WIDTHxHEIGHT"};
  }
  const auto parse_extent = [](std::string_view text) {
    std::size_t consumed = 0;
    const auto extent = std::stoull(std::string{text}, &consumed, 10);
    if (consumed != text.size() || extent == 0U || extent > 4'096U) {
      throw std::runtime_error{
          "display extents must be in the range 1..4096"};
    }
    return static_cast<std::uint32_t>(extent);
  };
  return DisplayGeometry{parse_extent(value.substr(0, separator)),
                         parse_extent(value.substr(separator + 1U))};
}

std::unique_ptr<Output> make_output(const std::vector<std::string> &args) {
  if (const auto path = option(args, "--output")) {
    return std::make_unique<Output>(*path);
  }
  return std::make_unique<Output>(std::cout);
}

const DeviceProfile& select_device_profile(
    const std::vector<std::string>& args) {
  const auto requested =
      option(args, "--device").value_or(
          std::string{DeviceProfile::default_profile().product_type});
  if (const auto* profile = DeviceProfile::find(requested)) {
    return *profile;
  }
  std::ostringstream message;
  message << "unknown device profile: " << requested << "; available:";
  for (const auto& profile : DeviceProfile::available_profiles()) {
    message << ' ' << profile.product_type;
  }
  throw std::runtime_error{message.str()};
}

void profile(const std::vector<std::string>& args, Output &output) {
  const auto &device = select_device_profile(args);
  std::ostringstream text;
  text << "product: " << device.product_type << '\n'
       << "board: " << device.board_config << '\n'
       << "model_number: " << device.model_number << '\n'
       << "soc: " << device.soc << '\n'
       << "cpu: " << device.cpu_core << " (" << device.instruction_set << ")\n"
       << "cpu_hz: " << device.cpu_hz << '\n'
       << "ram_bytes: " << device.ram_bytes << '\n'
       << "guest_physical_core_count: "
       << device.guest_cpu_topology.physical_core_count << '\n'
       << "guest_logical_cpu_count: "
       << device.guest_cpu_topology.logical_cpu_count << '\n'
       << "guest_cpu_clusters: " << device.guest_cpu_topology.cluster_count
       << '\n'
       << "display: " << device.display.width << 'x' << device.display.height
       << '\n'
       << "ui: " << device.user_interface.width << 'x'
       << device.user_interface.height;
  output.line(text.str());
}

void inspect(const std::vector<std::string> &args, Output &output) {
  const auto rootfs = option(args, "--rootfs");
  if (!rootfs) {
    throw std::runtime_error{"inspect requires --rootfs"};
  }
  const auto guest_binary = option(args, "--binary").value_or("/sbin/launchd");
  std::filesystem::path relative = guest_binary;
  if (relative.is_absolute()) {
    relative = relative.relative_path();
  }
  const auto host_path = std::filesystem::path{*rootfs} / relative;
  const auto image = MachOImage::parse(
      host_path,
      arm_architecture_for_model(select_device_profile(args).cpu_model));

  std::ostringstream text;
  text << "path: " << host_path.string() << '\n'
       << "cpu: " << mach_cpu_name(image.cpu_type(), image.cpu_subtype())
       << '\n'
       << "file_type: " << mach_file_type_name(image.file_type()) << '\n'
       << "load_commands: " << image.command_count() << '\n'
       << "entry: ";
  if (image.entry_point()) {
    text << "0x" << std::hex << *image.entry_point() << std::dec;
  } else {
    text << "unknown";
  }
  text << '\n' << "dyld: " << image.dynamic_linker().value_or("none") << '\n';
  for (const auto &segment : image.segments()) {
    text << "segment " << segment.name << " vm=0x" << std::hex
         << segment.vm_address << " size=0x" << segment.vm_size << " file=0x"
         << segment.file_offset << "+0x" << segment.file_size << std::dec
         << '\n';
    for (const auto &section : segment.sections) {
      text << "  section " << section.segment << ',' << section.name << " vm=0x"
           << std::hex << section.address << " size=0x" << section.size
           << " file=0x" << section.file_offset << " flags=0x" << section.flags
           << " reserved1=0x" << section.reserved1 << " reserved2=0x"
           << section.reserved2 << std::dec << '\n';
    }
  }
  for (const auto &dylib : image.dylibs()) {
    text << (dylib.prebound ? "prebound " : "dylib ") << dylib.path << '\n';
  }
  if (!image.unknown_commands().empty()) {
    text << "unknown_commands:";
    for (const auto command : image.unknown_commands()) {
      text << " 0x" << std::hex << command;
    }
    text << std::dec << '\n';
  }
  if (const auto pattern = option(args, "--symbols")) {
    for (const auto &symbol : image.symbols()) {
      if (symbol.name.find(*pattern) == std::string::npos)
        continue;
      text << "symbol " << symbol.name << " vm=0x" << std::hex << symbol.value
           << " type=0x" << static_cast<unsigned>(symbol.type) << " section=0x"
           << static_cast<unsigned>(symbol.section) << " desc=0x"
           << symbol.description << (symbol.thumb_definition() ? " thumb" : "")
           << std::dec << '\n';
    }
    for (const auto &stub : image.stubs()) {
      if (stub.symbol.find(*pattern) == std::string::npos)
        continue;
      text << "stub " << stub.symbol << " vm=0x" << std::hex << stub.address
           << " size=0x" << stub.size << std::dec << '\n';
    }
  }

  AddressSpace memory;
  image.map_into(memory);
  text << "mapped_pages: " << memory.mapped_page_count();
  output.line(text.str());
}

void catalog(const std::vector<std::string> &args, Output &output) {
  const auto rootfs = option(args, "--rootfs");
  if (!rootfs) throw std::runtime_error{"catalog requires --rootfs"};
  const auto architecture = arm_architecture_for_model(
      select_device_profile(args).cpu_model);
  ExecutableCatalog executable_catalog;
  const auto manifest = option(args, "--manifest").value_or(
      (host_cache_directory(args, std::filesystem::path{*rootfs}) /
       "executable-catalog.bin")
          .string());
  const auto had_manifest = executable_catalog.load(manifest);
  const auto summary = had_manifest
                           ? executable_catalog.refresh_tree(*rootfs, architecture)
                           : executable_catalog.register_tree(*rootfs, architecture);
  if (!executable_catalog.save(manifest)) {
    throw std::runtime_error{"failed to save executable catalog manifest: " +
                             manifest};
  }
  output.line(
      "[catalog] rootfs=" + *rootfs +
      " regular-files=" + std::to_string(summary.regular_files) +
      " macho-images=" + std::to_string(summary.mach_o_images) +
      " reused-macho-images=" +
      std::to_string(summary.reused_mach_o_images) +
      " shared-cache-generations=" +
      std::to_string(summary.dyld_shared_cache_generations) +
      " shared-cache-images=" +
      std::to_string(summary.dyld_shared_cache_images) +
      " failed-files=" + std::to_string(summary.failed_files) +
      " entries=" + std::to_string(executable_catalog.size()) +
      " reliable-entry-points=" +
      std::to_string(executable_catalog.reliable_entry_point_count()) +
      " manifest=" + manifest);
}

void firmware_prepare(const std::vector<std::string> &args, Output &output) {
  const auto rootfs = option(args, "--rootfs");
  if (!rootfs) throw std::runtime_error{"firmware prepare requires --rootfs"};
  const auto &device = select_device_profile(args);
  const auto cpu_model = make_arm_cpu_model(device.cpu_model, device.cpu_hz);
  const auto host_cache =
      host_cache_directory(args, std::filesystem::path{*rootfs});
  const auto manifest = option(args, "--manifest").value_or(
      (host_cache / "executable-catalog.bin").string());

  FirmwarePrepareLimits limits;
  limits.max_file_blocks =
      parse_prepare_count(args, "--prepare-file-blocks", 128U, 65'536U);
  limits.max_image_blocks =
      parse_prepare_count(args, "--prepare-image-blocks", 128U, 65'536U);
  limits.max_firmware_blocks =
      parse_prepare_count(args, "--prepare-firmware-blocks", 4096U,
                          1'000'000U);
  limits.max_file_time =
      parse_prepare_time(args, "--prepare-file-ms", 500U, 3'600'000U);
  limits.max_image_time =
      parse_prepare_time(args, "--prepare-image-ms", 500U, 3'600'000U);
  limits.max_firmware_time =
      parse_prepare_time(args, "--prepare-firmware-ms", 30'000U,
                         86'400'000U);
  const auto file_memory = parse_mib_value(
      option(args, "--prepare-file-memory-mib").value_or("128"),
      "--prepare-file-memory-mib", 1U, 4096U);
  const auto image_memory = parse_mib_value(
      option(args, "--prepare-image-memory-mib").value_or("128"),
      "--prepare-image-memory-mib", 1U, 4096U);
  const auto firmware_memory = parse_mib_value(
      option(args, "--prepare-firmware-memory-mib").value_or("512"),
      "--prepare-firmware-memory-mib", 1U, 16'384U);
  const auto file_storage = parse_mib_value(
      option(args, "--prepare-file-storage-mib").value_or("32"),
      "--prepare-file-storage-mib", 1U, 4096U);
  const auto image_storage = parse_mib_value(
      option(args, "--prepare-image-storage-mib").value_or("32"),
      "--prepare-image-storage-mib", 1U, 4096U);
  const auto firmware_storage = parse_mib_value(
      option(args, "--prepare-firmware-storage-mib").value_or("256"),
      "--prepare-firmware-storage-mib", 1U, 4096U);
  limits.max_file_memory_bytes = static_cast<std::size_t>(file_memory);
  limits.max_image_memory_bytes = static_cast<std::size_t>(image_memory);
  limits.max_firmware_memory_bytes = static_cast<std::size_t>(firmware_memory);
  limits.max_file_storage_bytes = static_cast<std::size_t>(file_storage);
  limits.max_image_storage_bytes = static_cast<std::size_t>(image_storage);
  limits.max_firmware_storage_bytes = static_cast<std::size_t>(firmware_storage);
  limits.artifact_resident_bytes = jit_artifact_memory_limit(args);
  const auto disk_mib = option(args, "--jit-artifact-disk-mib");
  if (disk_mib && *disk_mib == "0") {
    limits.artifact_persistence_bytes = 0U;
    limits.artifact_persistence_enabled = false;
  } else {
    limits.artifact_persistence_bytes = static_cast<std::size_t>(
        parse_mib_value(disk_mib.value_or("256"), "--jit-artifact-disk-mib",
                        1U, 4096U));
  }
  limits.artifact_minimum_free_bytes = static_cast<std::uintmax_t>(
      parse_mib_value(
          option(args, "--jit-artifact-min-free-mib").value_or("1024"),
          "--jit-artifact-min-free-mib", 0U, 16'384U));
  limits.force = flag(args, "--prepare-force");

  FirmwarePreparer preparer{
      std::filesystem::path{*rootfs}, std::filesystem::path{manifest},
      host_cache, cpu_model->architecture_version(), *cpu_model, limits};
  const auto stats = preparer.run();
  const auto status = stats.interrupted || stats.partial_files != 0U ||
                              stats.preparation_failures != 0U ||
                              stats.skipped_limits != 0U
                          ? "partial"
                          : "complete";
  output.line(
      "[firmware-prepare] rootfs=" + *rootfs + " manifest=" + manifest +
      " catalog-entries=" + std::to_string(stats.catalog_entries) +
      " regular-files=" + std::to_string(stats.catalog_scan.regular_files) +
      " macho-images=" + std::to_string(stats.catalog_scan.mach_o_images) +
      " reused-macho-images=" +
      std::to_string(stats.catalog_scan.reused_mach_o_images) +
      " shared-cache-images=" +
      std::to_string(stats.catalog_scan.dyld_shared_cache_images) +
      " failed-files=" + std::to_string(stats.catalog_scan.failed_files) +
      " reliable-entry-points=" +
      std::to_string(stats.reliable_entry_points) +
      " status=" + status);
  output.line(
      "[firmware-prepare-work] candidates=" +
      std::to_string(stats.candidates) + " skipped-dynamic=" +
      std::to_string(stats.skipped_dynamic_mappings) +
      " skipped-without-generation=" +
      std::to_string(stats.skipped_without_generation) + " skipped-limits=" +
      std::to_string(stats.skipped_limits) + " resumed=" +
      std::to_string(stats.resumed) + " files-processed=" +
      std::to_string(stats.files_processed) + " images-processed=" +
      std::to_string(stats.images_processed) + " completed=" +
      std::to_string(stats.completed_files) + " partial=" +
      std::to_string(stats.partial_files) + " failures=" +
      std::to_string(stats.preparation_failures) + " interrupted=" +
      std::to_string(stats.interrupted ? 1 : 0) + " storage-limited=" +
      std::to_string(stats.storage_limited ? 1 : 0) + " state-writes=" +
      std::to_string(stats.state_writes));
  output.line(
      "[precompile] target=portable-ir attempted=" +
      std::to_string(stats.blocks_attempted) + " generated=" +
      std::to_string(stats.portable_generated) + " artifact-hits=" +
      std::to_string(stats.portable_artifact_hits) + " deferred=" +
      std::to_string(stats.deferred) + " unstable=" +
      std::to_string(stats.unstable) + " failed=" +
      std::to_string(stats.failed) + " deadline-stops=" +
      std::to_string(stats.deadline_stops) + " prepared-memory-bytes=" +
      std::to_string(stats.prepared_memory_bytes));
  output.line(
      "[jit-artifact] resident-bytes=" +
      std::to_string(stats.artifact_stats.resident_bytes) +
      " writeback-pending-bytes=" +
      std::to_string(stats.artifact_stats.writeback_pending_bytes) +
      " disk-bytes=" + std::to_string(stats.artifact_stats.disk_bytes) +
      " disk-hits=" + std::to_string(stats.artifact_stats.disk_hits) +
      " memory-hits=" + std::to_string(stats.artifact_stats.memory_hits) +
      " evictions=" + std::to_string(stats.artifact_stats.evictions) +
      " quota-evictions=" +
      std::to_string(stats.artifact_stats.quota_evictions));
}

template <std::size_t Size>
void append_word(std::array<std::byte, Size> &code, std::size_t offset,
                 std::uint32_t word) {
  for (std::size_t i = 0; i < 4; ++i) {
    code[offset + i] = static_cast<std::byte>((word >> (i * 8U)) & 0xffU);
  }
}

void disasm(const std::vector<std::string> &args, Output &output) {
  const auto rootfs = option(args, "--rootfs");
  const auto binary = option(args, "--binary");
  const auto symbol_name = option(args, "--symbol");
  const auto address_option = option(args, "--address");
  if (!rootfs || !binary || (!symbol_name && !address_option) ||
      (symbol_name && address_option)) {
    throw std::runtime_error{"disasm requires --rootfs, --binary, and exactly "
                             "one of --symbol/--address"};
  }
  std::filesystem::path relative = *binary;
  if (relative.is_absolute())
    relative = relative.relative_path();
  const auto image = MachOImage::parse(
      std::filesystem::path{*rootfs} / relative,
      arm_architecture_for_model(select_device_profile(args).cpu_model));
  const MachSymbol *symbol = nullptr;
  std::uint32_t start_address = 0;
  if (symbol_name) {
    symbol = image.find_symbol(*symbol_name);
    if (symbol == nullptr || symbol->value == 0) {
      throw std::runtime_error{"defined symbol not found: " + *symbol_name};
    }
    start_address = symbol->value;
  } else {
    start_address =
        static_cast<std::uint32_t>(std::stoul(*address_option, nullptr, 0));
    for (const auto &candidate : image.symbols()) {
      if (candidate.value != 0 && candidate.value <= start_address &&
          (symbol == nullptr || candidate.value > symbol->value)) {
        symbol = &candidate;
      }
    }
  }
  const auto count = static_cast<std::size_t>(
      std::stoul(option(args, "--count").value_or("8")));
  const auto thumb =
      std::find(args.begin(), args.end(), "--thumb") != args.end();
  std::ostringstream text;
  if (symbol != nullptr) {
    text << symbol->name;
    if (start_address != symbol->value) {
      text << "+0x" << std::hex << (start_address - symbol->value) << std::dec;
    }
    text << " @ ";
  }
  text << "0x" << std::hex << start_address << std::dec << '\n';
  for (std::size_t index = 0; index < count; ++index) {
    if (thumb) {
      const auto address =
          start_address + static_cast<std::uint32_t>(index * 2U);
      const auto instruction = image.read_vm_u16(address);
      if (!instruction)
        break;
      text << "0x" << std::hex << std::setw(8) << std::setfill('0') << address
           << "  " << std::setw(4) << *instruction << "      "
           << Dynarmic::A32::DisassembleThumb16(*instruction) << '\n';
    } else {
      const auto address =
          start_address + static_cast<std::uint32_t>(index * 4U);
      const auto instruction = image.read_vm_u32(address);
      if (!instruction)
        break;
      text << "0x" << std::hex << std::setw(8) << std::setfill('0') << address
           << "  " << std::setw(8) << *instruction << "  "
           << Dynarmic::A32::DisassembleArm(*instruction);
      if ((*instruction & 0x0f000000U) == 0x0b000000U) {
        auto displacement = static_cast<std::int32_t>(*instruction << 8U) >> 6U;
        const auto target =
            address + 8U + static_cast<std::uint32_t>(displacement);
        if (const auto *stub = image.find_stub(target)) {
          text << " ; " << stub->symbol;
        }
      }
      text << '\n';
    }
  }
  output.write(text.str());
}

void smoke(const std::vector<std::string> &args, Output &output) {
  const auto core_count_string = option(args, "--cores").value_or("2");
  const auto core_count =
      static_cast<std::size_t>(std::stoul(core_count_string));
  if (core_count == 0 || core_count > maximum_virtual_processors) {
    throw std::runtime_error{"--cores must be in the range 1.." +
                             std::to_string(maximum_virtual_processors)};
  }

  AddressSpace memory;
  constexpr std::uint32_t code_address = 0x1000;
  memory.map(code_address, AddressSpace::page_size,
             MemoryPermission::Read | MemoryPermission::Write |
                 MemoryPermission::Execute);
  std::array<std::byte, 8> code{};
  append_word(code, 0, 0xe2800001U); // add r0, r0, #1
  append_word(code, 4, 0xef000080U); // svc #0x80 (Darwin syscall gate)
  memory.copy_in(code_address, code);

  Dynarmic::ExclusiveMonitor shared_exclusive_monitor{core_count};
  auto shared_exclusive_address_resolver =
      std::make_shared<GuestExclusiveAddressResolver>();
  CpuCluster cluster{
      core_count,
      core_count,
      memory,
      core_count,
      default_arm_cpu_model(),
      shared_exclusive_monitor,
      0,
      {},
      shared_exclusive_address_resolver};
  cluster.set_jit_code_cache_size(jit_code_cache_size(args));
  for (std::size_t index = 0; index < cluster.size(); ++index) {
    cluster.cpu(index).registers()[0] = static_cast<std::uint32_t>(index * 100);
    cluster.cpu(index).registers()[15] = code_address;
    cluster.cpu(index).set_cpsr(0x10); // ARM user mode, ARM state
  }
  const auto results = cluster.run_parallel(16);

  std::ostringstream text;
  text << "Dynarmic ARMv6 parallel smoke test: " << core_count
       << " virtual CPU(s)\n";
  if (core_count > 1) {
    text << "mode: exact; execution-slot LDREX state resolves through "
            "GuestPageBacking identity for shared-page atomics\n";
  }
  for (std::size_t index = 0; index < cluster.size(); ++index) {
    text << "cpu" << index << " r0=" << cluster.cpu(index).registers()[0]
         << " pc=0x" << std::hex << cluster.cpu(index).registers()[15]
         << std::dec << " svc="
         << (results[index].svc ? std::to_string(*results[index].svc) : "none")
         << '\n';
    const auto expected = static_cast<std::uint32_t>(index * 100 + 1);
    if (cluster.cpu(index).registers()[0] != expected ||
        results[index].svc != std::optional<std::uint32_t>{0x80}) {
      throw std::runtime_error{
          "Dynarmic smoke test produced an unexpected CPU state"};
    }
  }
  text << "status: ok";
  output.line(text.str());
}

void benchmark(const std::vector<std::string> &args, Output &output) {
  if (args.empty() || args.front() != "arm") {
    throw std::runtime_error{"benchmark requires the 'arm' baseline"};
  }
  const auto value = option(args, "--iterations").value_or("1000000");
  std::size_t consumed = 0;
  const auto parsed = std::stoull(value, &consumed, 10);
  if (consumed != value.size() || parsed == 0 ||
      parsed > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error{
        "--iterations must be in the range 1..4294967295"};
  }
  const auto iterations = static_cast<std::uint32_t>(parsed);

  AddressSpace memory;
  constexpr std::uint32_t code_address = 0x1000;
  if (!memory.map(code_address, AddressSpace::page_size,
                  MemoryPermission::Read | MemoryPermission::Write |
                      MemoryPermission::Execute)) {
    throw std::runtime_error{"ARM benchmark code mapping failed"};
  }
  std::array<std::byte, 16> code{};
  append_word(code, 0, 0xe3a01000U);  // mov r1, #0
  append_word(code, 4, 0xe2811001U);  // add r1, r1, #1
  append_word(code, 8, 0xe2500001U);  // subs r0, r0, #1
  append_word(code, 12, 0x1afffffcU); // bne 0x1004
  if (!memory.copy_in(code_address, code)) {
    throw std::runtime_error{"ARM benchmark code upload failed"};
  }
  constexpr std::uint32_t svc_address = code_address + sizeof(code);
  const std::array<std::byte, 4> svc{
      std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0xef}};
  if (!memory.copy_in(svc_address, svc)) {
    throw std::runtime_error{"ARM benchmark SVC upload failed"};
  }

  CpuCluster cluster{1, memory};
  cluster.set_jit_code_cache_size(jit_code_cache_size(args));
  auto &cpu = cluster.cpu(0);
  cpu.registers()[0] = iterations;
  cpu.registers()[15] = code_address;
  cpu.set_cpsr(0x10);
  const auto tick_budget = static_cast<std::uint64_t>(iterations) * 16U + 32U;
  const auto started = std::chrono::steady_clock::now();
  const auto result = cpu.run(tick_budget);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  if (cpu.registers()[0] != 0 || cpu.registers()[1] != iterations ||
      result.svc != std::optional<std::uint32_t>{0x80}) {
    throw std::runtime_error{
        "ARM benchmark produced an unexpected CPU state"};
  }
  const auto elapsed_nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  const auto iterations_per_second =
      elapsed_nanoseconds > 0
          ? static_cast<std::uint64_t>(
                static_cast<long double>(iterations) * 1'000'000'000.0L /
                static_cast<long double>(elapsed_nanoseconds))
          : 0U;
  output.line("[benchmark] baseline=arm iterations=" +
              std::to_string(iterations) +
              " ticks=" + std::to_string(result.ticks_consumed) +
              " elapsed-ns=" + std::to_string(elapsed_nanoseconds) +
              " jit-cache-mib=" +
              std::to_string(jit_code_cache_size(args) / 1024U / 1024U) +
              " iterations-per-second=" +
              std::to_string(iterations_per_second) + " status=ok");
}

void boot(const std::vector<std::string> &args, Output &output) {
  constexpr std::string_view springboard_boot_path =
      "/System/Library/CoreServices/SpringBoard.app/SpringBoard";
  const auto rootfs = option(args, "--rootfs");
  if (!rootfs) {
    throw std::runtime_error{"boot requires --rootfs"};
  }
  const auto host_cache =
      host_cache_directory(args, std::filesystem::path{*rootfs});
  const auto catalog_manifest = option(args, "--catalog").value_or(
      (host_cache / "executable-catalog.bin").string());
  auto device = select_device_profile(args);
  ExecutableCatalog executable_catalog;
  bool catalog_loaded = executable_catalog.load(catalog_manifest);
  std::string catalog_source = catalog_loaded ? "manifest" : "fallback";
  if (!catalog_loaded) {
    try {
      const auto summary = executable_catalog.register_tree(
          *rootfs, arm_architecture_for_model(device.cpu_model));
      catalog_loaded = true;
      catalog_source = "startup-scan";
      const auto manifest_saved = executable_catalog.save(catalog_manifest);
      output.line(
          "[catalog] startup-scan=complete regular-files=" +
          std::to_string(summary.regular_files) + " macho-images=" +
          std::to_string(summary.mach_o_images) + " failed-files=" +
          std::to_string(summary.failed_files) + " entries=" +
          std::to_string(executable_catalog.size()) +
          " reliable-entry-points=" +
          std::to_string(executable_catalog.reliable_entry_point_count()) +
          " manifest-save=" + (manifest_saved ? "ok" : "failed"));
    } catch (const std::exception &error) {
      output.line("[catalog] startup-scan=failed error=" +
                  std::string{error.what()});
    }
  }
  output.line("[catalog] manifest=" + catalog_manifest +
              " status=" + (catalog_loaded ? "loaded" : "fallback") +
              " source=" + catalog_source +
              " entries=" + std::to_string(executable_catalog.size()));
  auto *catalog_index = catalog_loaded ? &executable_catalog : nullptr;
  const auto gles_backend = parse_gles_backend(args);
  configure_gles_pipeline_cache(
      host_cache / "vulkan-pipeline-cache.bin");
  configure_gles_backend(gles_backend);
  const auto binary = option(args, "--binary").value_or("/sbin/launchd");
  if (const auto display_size = option(args, "--display-size")) {
    device.display = parse_display_geometry(*display_size);
  }
  output.line("[device] product=" + std::string{device.product_type} +
              " display=" + std::to_string(device.display.width) + "x" +
              std::to_string(device.display.height) + " ui=" +
              std::to_string(device.user_interface.width) + "x" +
              std::to_string(device.user_interface.height));
  const auto activation_value =
      option(args, "--activation").value_or("activated");
  const auto activation = parse_lockdown_activation(activation_value);
  if (!activation) {
    throw std::runtime_error{
        "--activation must be activated, unactivated, or preserve"};
  }
  const auto lockdown_profile = detect_lockdown_firmware_profile(
      *rootfs, arm_architecture_for_model(device.cpu_model));
  const auto activation_result =
      apply_lockdown_profile(*rootfs, *activation, lockdown_profile);
  output.line("[device-state] activation=" + activation_value +
              " path=" + activation_result.path.string() +
              " changed=" + std::to_string(activation_result.changed) +
              " registration-profile=" +
              std::to_string(lockdown_profile.registration_state) +
              " brick-profile=" +
              std::to_string(lockdown_profile.brick_state));
  const auto activation_override =
      *activation == LockdownActivation::Preserve
          ? std::optional<bool>{}
          : std::optional<bool>{
                *activation == LockdownActivation::Activated};
  if (activation_override && *activation_override) {
    // The stock daemon has a firmware-supported development-board escape
    // hatch for a device without a baseband/activation record. Selecting it
    // only for the explicit activated simulator profile leaves preserve mode
    // as the authentic retail contract.
    device.hardware_model = device.activation_hardware_model;
  }
  const auto ticks_option = option(args, "--ticks");
  const auto bounded_execution = ticks_option.has_value();
  const auto ticks = ticks_option ? std::stoull(*ticks_option)
                                  : std::numeric_limits<std::uint64_t>::max();
  const auto default_processor_count =
      static_cast<std::size_t>(device.guest_cpu_topology.logical_cpu_count);
  if (!device.guest_cpu_topology.valid()) {
    throw std::runtime_error{"device profile has invalid guest CPU topology"};
  }
  const auto cpu_model =
      make_arm_cpu_model(device.cpu_model, device.cpu_hz);
  const auto guest_architecture = cpu_model->architecture_version();
  const auto guest_ticks_per_second =
      cpu_model->ticks_per_second();
  GuestTickClock guest_tick_clock{guest_ticks_per_second};
  const auto explicit_processor_count = option(args, "--cores");
  const auto guest_processor_count = static_cast<std::size_t>(std::stoul(
      explicit_processor_count.value_or(std::to_string(default_processor_count))));
  if (guest_processor_count == 0 ||
      guest_processor_count > maximum_virtual_processors) {
    throw std::runtime_error{"--cores must be in the range 1.." +
                             std::to_string(maximum_virtual_processors)};
  }
  if (explicit_processor_count) {
    output.line(
        "[cpu] mode=stress/dev cores=" +
        std::to_string(guest_processor_count) +
        " profile-cores=" + std::to_string(default_processor_count) +
        " warning=\"--cores overrides the device topology; execution-slot "
        "LDREX state follows the host slot and process-local "
        "ExclusiveMonitor does not model cross-process shared-page "
        "atomics\"");
  } else {
    output.line("[cpu] mode=faithful guest-cores=" +
                std::to_string(guest_processor_count) +
                " physical-cores=" +
                std::to_string(device.guest_cpu_topology.physical_core_count) +
                " topology-cache-id=" +
                std::to_string(device.guest_cpu_topology.cache_topology_id));
  }
  const auto configured_jit_code_cache_size =
      jit_code_cache_size(args);
  const auto jit_cache_budget = jit_code_cache_budget(args);
  output.line("[jit] code-cache-mib=" +
              std::to_string(configured_jit_code_cache_size /
                             1024U / 1024U));
  output.line(
      "[jit] host-memory physical-mib=" +
      std::to_string(jit_cache_budget.memory.physical_bytes /
                     bytes_per_mebibyte) +
      " available-mib=" +
      std::to_string(jit_cache_budget.memory.available_bytes /
                     bytes_per_mebibyte) +
      " rss-mib=" +
      std::to_string(jit_cache_budget.memory.rss_bytes / bytes_per_mebibyte) +
      " cgroup-limit-mib=" +
      (!jit_cache_budget.memory.cgroup_limit_known
           ? std::string{"unlimited"}
           : std::to_string(jit_cache_budget.memory.cgroup_limit_bytes /
                            bytes_per_mebibyte)) +
      " cgroup-current-mib=" +
      std::to_string(jit_cache_budget.memory.cgroup_current_bytes /
                     bytes_per_mebibyte) +
      " budget-source=" +
      (jit_cache_budget.explicit_override ? "explicit" : "adaptive") +
      " physical-state=" +
      (jit_cache_budget.memory.physical_known ? "known" : "unavailable") +
      " available-state=" +
      (jit_cache_budget.memory.available_known ? "known" : "unavailable") +
      " rss-state=" +
      (jit_cache_budget.memory.rss_known ? "known" : "unavailable") +
      " cgroup-limit-state=" +
      (jit_cache_budget.memory.cgroup_limit_known ? "known" : "unavailable") +
      " cgroup-current-state=" +
      (jit_cache_budget.memory.cgroup_current_known ? "known" : "unavailable") +
      " pressure=" +
      std::string{host_memory_pressure_name(jit_cache_budget.memory)} +
      " total-budget-mib=" +
      std::to_string(jit_cache_budget.total_bytes / bytes_per_mebibyte));
  std::unique_ptr<GuestSliceWorkerPool> guest_slice_workers;
  if (guest_processor_count > 1) {
    guest_slice_workers =
        std::make_unique<GuestSliceWorkerPool>(guest_processor_count);
  }
  const auto network_policy_value =
      option(args, "--network").value_or("host");
  const auto network_policy = parse_host_network_policy(network_policy_value);
  if (!network_policy) {
    throw std::runtime_error{"--network must be isolated, loopback, or host"};
  }
  const auto airport_configuration = NetworkPreferencesAirport{
      .interface_name = "en0",
      .mac_address = wifi_interface_mac_address,
      .ipv4 = NetworkPreferencesIpv4{
          .address = virtual_network::client_address,
          .netmask = virtual_network::netmask,
          .gateway = virtual_network::gateway_address,
          .dns_servers = {virtual_network::dns_proxy_address},
      },
  };
  const auto network_preferences =
      ensure_network_preferences(*rootfs, airport_configuration);
  auto preferred_wifi_networks =
      network_preferences.preferred_wifi_networks;
  output.line(
      "[device-state] airport-service=" +
      (network_preferences.service_identifier.empty()
           ? std::string{"unavailable"}
           : network_preferences.service_identifier) +
      " path=" + network_preferences.path.string() +
      " supported=" + std::to_string(network_preferences.supported) +
      " changed=" + std::to_string(network_preferences.changed));
  const auto display_mode = option(args, "--display").value_or("headless");
  if (display_mode != "headless" && display_mode != "sdl") {
    throw std::runtime_error{"--display must be headless or sdl"};
  }
  if (!bounded_execution && display_mode == "sdl" &&
      std::find(args.begin(), args.end(), "--verbose") == args.end()) {
    output.set_verbose(false);
  }
  std::unique_ptr<SdlDisplay> sdl_display;
  std::unique_ptr<FrameFilePresenter> frame_file_presenter;
  std::unique_ptr<TouchReplay> touch_replay;
  std::unique_ptr<LiveControl> live_control;
  LiveButtonScheduler live_button_scheduler;
  LiveTouchScheduler live_touch_scheduler;
  if (display_mode == "sdl") {
    if (!SdlDisplay::available()) {
      throw std::runtime_error{
          "--display sdl requested, but SDL2 support is not built"};
    }
    sdl_display = std::make_unique<SdlDisplay>(device.display,
                                               device.user_interface);
    if (const auto presenter =
            sdl_display->vulkan_presenter_configuration()) {
      configure_gles_vulkan_presenter(*presenter);
    }
  }
  auto gles_renderer = shared_gles_renderer();
  if (sdl_display)
    sdl_display->set_host_graphics(gles_renderer);
  output.line("[gles] requested=" +
              std::string{gles_backend_name(gles_backend)} + " renderer=\"" +
              std::string{gles_renderer->name()} + "\" accelerated=" +
              std::to_string(gles_renderer->accelerated()) +
              " software-fallback=" +
              (gles_renderer->software_fallback_allowed() ? "allowed"
                                                           : "disabled") +
              " direct-present=" +
              (gles_renderer->native_presentation_available() ? "yes"
                                                               : "no"));
  struct RendererLifetime {
    std::shared_ptr<GlesRenderer> &renderer;
    SdlDisplay *display;
    ~RendererLifetime() {
      if (display)
        display->set_host_graphics({});
      renderer.reset();
      shutdown_gles_renderer();
    }
  } renderer_lifetime{gles_renderer, sdl_display.get()};
  if (const auto path = option(args, "--frame-output")) {
    frame_file_presenter = std::make_unique<FrameFilePresenter>(*path);
  }
  if (const auto path = option(args, "--touch-replay")) {
    touch_replay = std::make_unique<TouchReplay>(*path);
  }
  if (std::find(args.begin(), args.end(), "--control-stdin") != args.end()) {
    live_control = std::make_unique<LiveControl>(0, device.user_interface);
    output.line("[control] ready; use help for commands");
  }
  std::optional<std::uint16_t> gdb_port;
  if (const auto value = option(args, "--gdb")) {
    const auto parsed = std::stoul(*value);
    if (parsed == 0 || parsed > std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error{
          "--gdb must be a TCP port in the range 1..65535"};
    }
    gdb_port = static_cast<std::uint16_t>(parsed);
  }
  std::optional<std::uint32_t> watch_address;
  if (const auto value = option(args, "--watch-address")) {
    const auto parsed = std::stoull(*value, nullptr, 0);
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
      throw std::runtime_error{
          "--watch-address exceeds the 32-bit guest address space"};
    }
    watch_address = static_cast<std::uint32_t>(parsed);
  }
  const auto baseband_input_path = option(args, "--baseband-input");
  const auto baseband_output_path = option(args, "--baseband-output");
  std::optional<std::ofstream> baseband_capture_stream;
  std::uint64_t baseband_capture_bytes{};
  if (baseband_output_path) {
    baseband_capture_stream.emplace(
        *baseband_output_path, std::ios::binary | std::ios::trunc);
    if (!*baseband_capture_stream) {
      throw std::runtime_error{"cannot open baseband capture output: " +
                               *baseband_output_path};
    }
  }
  const auto baseband_input =
      baseband_input_path
          ? bsd::baseband_device::load_replay_file(*baseband_input_path)
          : std::vector<std::byte>{};
  // A replay input is the only fixture that supplies a device-side transport
  // contract. Without one, retain the common Offline/no-modem policy so a
  // missing radio cannot block the rest of the system.
  device.baseband_transport =
      baseband_input_path
          ? BasebandTransportProfile::Virtual
          : device.baseband_transport;

  auto initial_memory = std::make_unique<AddressSpace>();
  initial_memory->set_parallel_access(guest_processor_count > 1);
  ProcessLoader loader{*rootfs, *initial_memory, guest_architecture,
                       catalog_index};
  std::vector<std::string> initial_environment{
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/var/root",
      "SHELL=/bin/sh"};
  auto process = loader.load(binary, {}, initial_environment);
  std::unordered_set<ContentIdentity, ContentIdentityHash>
      boot_image_identities;
  boot_image_identities.insert(process.executable.content_identity());
  if (catalog_index != nullptr) {
    auto springboard_host_path = std::filesystem::path{*rootfs};
    springboard_host_path /=
        std::filesystem::path{springboard_boot_path}.relative_path();
    if (const auto *springboard =
            catalog_index->find_path(springboard_host_path)) {
      boot_image_identities.insert(springboard->content_identity);
    }
  }
  output.line("[jit-artifact] boot-image-roots=" +
              std::to_string(boot_image_identities.size()));
  // Dynarmic's global monitor indexes reservations by processor id. Reserve
  // disjoint ranges for boot-created Guest processes so same-address shared
  // mappings can invalidate reservations across process boundaries.
  Dynarmic::ExclusiveMonitor shared_exclusive_monitor{
      maximum_shared_monitor_slots};
  auto shared_exclusive_address_resolver =
      std::make_shared<GuestExclusiveAddressResolver>();
  std::size_t next_shared_monitor_slot{};
  const auto allocate_shared_monitor_slots = [&]() {
    if (guest_processor_count >
        maximum_shared_monitor_slots - next_shared_monitor_slot) {
      throw std::runtime_error{
          "shared exclusive monitor processor capacity exhausted"};
    }
    const auto base = next_shared_monitor_slot;
    next_shared_monitor_slot += guest_processor_count;
    return base;
  };
  JitCodeCacheGovernor jit_code_cache_governor{
      configured_jit_code_cache_size, jit_cache_budget.total_bytes};
  jit_code_cache_governor.set_pressure_limited(
      host_memory_is_pressured(jit_cache_budget.memory));
  output.line(
      "[jit] global-code-cache-budget-mib=" +
      std::to_string(jit_code_cache_governor.total_budget() / 1024U / 1024U));
  output.line(
      "[jit] emergency-code-cache-budget-mib=" +
      std::to_string(jit_code_cache_governor.emergency_budget() /
                     1024U / 1024U));
  output.line(
      "[jit] shared-slab-cache-mib=boot-critical:" +
      std::to_string(jit_code_cache_governor.shared_slab_cap(
                         JitCodeCacheClass::BootCritical) /
                     1024U / 1024U) +
      " foreground:" +
      std::to_string(jit_code_cache_governor.shared_slab_cap(
                         JitCodeCacheClass::Foreground) /
                     1024U / 1024U) +
      " background:" +
      std::to_string(jit_code_cache_governor.shared_slab_cap(
                         JitCodeCacheClass::Background) /
                     1024U / 1024U) +
      " pressure-limited=" +
      std::to_string(jit_code_cache_governor.pressure_limited() ? 1 : 0));
  RuntimeReaper runtime_reaper;
  std::vector<std::unique_ptr<Runtime>> runtimes;
  RuntimeIndex runtime_index;
  HostResourceBudget host_resource_budget;
  const auto host_concurrency = std::max<unsigned>(
      1U, std::thread::hardware_concurrency());
  const auto reserved_guest_workers = std::max<std::size_t>(
      1U, guest_processor_count);
  const auto spare_host_workers =
      host_concurrency > reserved_guest_workers
          ? static_cast<std::size_t>(host_concurrency) - reserved_guest_workers
          : 0U;
  host_resource_budget.worker_count = std::clamp(
      spare_host_workers, std::size_t{0}, maximum_background_workers);
  output.line("[host] background-workers=" +
              std::to_string(host_resource_budget.worker_count) +
              " host-concurrency=" + std::to_string(host_concurrency) +
              " guest-workers-reserved=" +
              std::to_string(reserved_guest_workers));
  HostResourceController host_resources{host_resource_budget};
  JitTranslationProfileStore translation_profiles{
      host_cache /
      "jit-translation-profiles"};
  JitArtifactLimits jit_artifact_limits;
  jit_artifact_limits.resident_bytes = jit_artifact_memory_limit(args);
  constexpr auto minimum_artifact_free_bytes =
      std::uintmax_t{128U} * 1024U * 1024U;
  constexpr auto maximum_artifact_disk_bytes =
      std::uintmax_t{4U} * 1024U * 1024U * 1024U;
  jit_artifact_limits.minimum_free_bytes = minimum_artifact_free_bytes;
  const auto configured_disk_mib = option(args, "--jit-artifact-disk-mib");
  const auto artifact_filesystem =
      nearest_existing_filesystem_path(host_cache);
  if (configured_disk_mib) {
    const auto configured_bytes = parse_mib_value(
        *configured_disk_mib, "--jit-artifact-disk-mib", 0U, 4096U);
    if (configured_bytes == 0U) {
      jit_artifact_limits.persistence_enabled = false;
    } else {
      jit_artifact_limits.persistence_bytes = static_cast<std::size_t>(
          std::min(configured_bytes, maximum_artifact_disk_bytes));
    }
  } else {
    std::error_code disk_space_error;
    const auto disk_space = std::filesystem::space(
        artifact_filesystem, disk_space_error);
    if (!disk_space_error &&
        disk_space.available > minimum_artifact_free_bytes) {
      const auto available_budget =
          disk_space.available - minimum_artifact_free_bytes;
      jit_artifact_limits.persistence_bytes = static_cast<std::size_t>(
          std::min(available_budget, maximum_artifact_disk_bytes));
    } else {
      jit_artifact_limits.persistence_enabled = false;
    }
  }
  output.line(
      "[jit-artifact] memory-mib=" +
      std::to_string(jit_artifact_limits.resident_bytes / 1024U / 1024U) +
      " writeback-mib=" +
      std::to_string(jit_artifact_limits.writeback_bytes / 1024U / 1024U) +
      " disk-mib=" +
      std::to_string(jit_artifact_limits.persistence_bytes / 1024U / 1024U) +
      " persistence=" +
      (jit_artifact_limits.persistence_enabled ? "enabled" : "disabled") +
      " filesystem=" +
      (artifact_filesystem.empty() ? std::string{"unavailable"}
                                    : artifact_filesystem.string()));
  auto jit_artifacts = std::make_shared<JitArtifactStore>(
      host_cache / "jit-artifacts.bin", jit_artifact_limits);
  std::shared_ptr<HostWorkToken> artifact_compaction_task;
  const auto precompile_phase_for_process =
      [springboard_boot_path](std::string_view executable_path) {
        if (executable_path == springboard_boot_path) {
          return JitPrecompilePhase::SystemUi;
        }
        if (is_application_executable_path(executable_path)) {
          return JitPrecompilePhase::ForegroundApplication;
        }
        return JitPrecompilePhase::StartupService;
      };
  constexpr std::size_t maximum_catalog_offline_compile_queue = 64;
  constexpr std::size_t maximum_catalog_preexec_blocks = 64;
  constexpr std::uint64_t catalog_preexec_budget_nanoseconds = 4'000'000;
  std::deque<ContentIdentity> pending_catalog_compiles;
  struct PrecompileOutcomeCounters {
    std::atomic<std::uint64_t> attempted{};
    std::atomic<std::uint64_t> native_compiled{};
    std::atomic<std::uint64_t> portable_generated{};
    std::atomic<std::uint64_t> portable_artifact_hits{};
    std::atomic<std::uint64_t> artifact_imported{};
    std::atomic<std::uint64_t> artifact_probe_hits{};
    std::atomic<std::uint64_t> shared_slab_hits{};
    std::atomic<std::uint64_t> deferred{};
    std::atomic<std::uint64_t> unstable{};
    std::atomic<std::uint64_t> cache_full{};
    std::atomic<std::uint64_t> failed{};
    std::atomic<std::uint64_t> deadline_stops{};
  } precompile_outcomes;
  enum class PrecompileScheduleSkip : std::uint8_t {
    NoRuntime,
    TaskBusy,
    NoPhase,
    MemoryPressure,
    DisplayQuiet,
    GuestNotIdle,
    DeadlineReserve,
    ZeroBudget,
    HostRejected,
    Count,
  };
  constexpr auto precompile_schedule_skip_count =
      static_cast<std::size_t>(PrecompileScheduleSkip::Count);
  struct PrecompileBudgetDecision {
    std::uint64_t budget{};
    PrecompileScheduleSkip zero_reason{PrecompileScheduleSkip::ZeroBudget};
  };
  std::array<std::uint64_t, precompile_schedule_skip_count>
      precompile_schedule_skips{};
  const auto record_precompile_schedule_skip =
      [&precompile_schedule_skips](PrecompileScheduleSkip reason) {
        ++precompile_schedule_skips[static_cast<std::size_t>(reason)];
      };
  const auto record_precompile_outcomes =
      [&precompile_outcomes](const JitPrecompileBatchResult &result) {
        precompile_outcomes.attempted.fetch_add(result.attempted,
                                                 std::memory_order_relaxed);
        precompile_outcomes.native_compiled.fetch_add(
            result.native_compiled, std::memory_order_relaxed);
        precompile_outcomes.portable_generated.fetch_add(
            result.portable_generated, std::memory_order_relaxed);
        precompile_outcomes.portable_artifact_hits.fetch_add(
            result.portable_artifact_hits, std::memory_order_relaxed);
        precompile_outcomes.artifact_imported.fetch_add(
            result.artifact_imported, std::memory_order_relaxed);
        precompile_outcomes.artifact_probe_hits.fetch_add(
            result.artifact_probe_hits, std::memory_order_relaxed);
        precompile_outcomes.shared_slab_hits.fetch_add(
            result.shared_slab_hits, std::memory_order_relaxed);
        precompile_outcomes.deferred.fetch_add(result.deferred,
                                               std::memory_order_relaxed);
        precompile_outcomes.unstable.fetch_add(result.unstable,
                                                std::memory_order_relaxed);
        precompile_outcomes.cache_full.fetch_add(result.cache_full,
                                                  std::memory_order_relaxed);
        precompile_outcomes.failed.fetch_add(result.failed,
                                              std::memory_order_relaxed);
        precompile_outcomes.deadline_stops.fetch_add(
            result.deadline_stops, std::memory_order_relaxed);
      };
  const auto assign_jit_process_profile =
      [&translation_profiles, &catalog_index, &boot_image_identities,
       &pending_catalog_compiles, &output,
       &jit_code_cache_governor, springboard_boot_path](
          Runtime &runtime, const LoadedProcess &loaded,
          JitPrecompilePhase phase) {
        runtime.precompile_phase = phase;
        const auto retention =
            boot_image_identities.contains(
                loaded.executable.content_identity()) ||
                    loaded.executable_path == springboard_boot_path
                ? JitArtifactRetention::BootWorkingSet
                : JitArtifactRetention::Normal;
        const auto cache_class =
            retention == JitArtifactRetention::BootWorkingSet
                ? JitCodeCacheClass::BootCritical
                : phase == JitPrecompilePhase::ForegroundApplication
                    ? JitCodeCacheClass::Foreground
                    : JitCodeCacheClass::Background;
        runtime.jit_cache_class = cache_class;
        if (runtime.jit_cache_reservation) {
          const auto shared_slab = jit_code_cache_governor.reclassify(
              *runtime.jit_cache_reservation, cache_class,
              runtime.fresh_spawn_address_space);
          // A freshly-created spawn has not instantiated Dynarmic yet, so
          // its role-specific quota can still resize the configured cache.
          // An ordinary exec may already own a live emitter; its reservation
          // is still reclassified, while Dynarmic keeps its immutable size.
          if (shared_slab && runtime.fresh_spawn_address_space) {
            runtime.cpus->set_jit_code_cache_size(*shared_slab);
          }
        }
        if (retention == JitArtifactRetention::BootWorkingSet) {
          boot_image_identities.insert(
              loaded.executable.content_identity());
        }
        runtime.cpus->set_jit_artifact_retention(retention);
        runtime.cpus->set_translation_profile(
            translation_profiles.profile_for(
                loaded.executable.content_identity()),
            phase);
        if (catalog_index == nullptr) return false;
        std::vector<std::uint64_t> entry_points;
        const auto append_entry_points = [&](const MachOImage &image) {
          const auto *entry = catalog_index->find(image.content_identity());
          if (entry == nullptr) return;
          entry_points.insert(entry_points.end(),
                              entry->reliable_entry_points.begin(),
                              entry->reliable_entry_points.end());
        };
        append_entry_points(loaded.executable);
        append_entry_points(loaded.dynamic_linker);
        runtime.cpus->add_precompile_entries(entry_points, phase);
        const auto consume_pending_identity = [&](const ContentIdentity &identity) {
          const auto pending = std::find(pending_catalog_compiles.begin(),
                                         pending_catalog_compiles.end(), identity);
          if (pending == pending_catalog_compiles.end()) return false;
          pending_catalog_compiles.erase(pending);
          return true;
        };
        const auto executable_pending =
            consume_pending_identity(loaded.executable.content_identity());
        const auto linker_pending =
            consume_pending_identity(loaded.dynamic_linker.content_identity());
        if (executable_pending || linker_pending) {
          output.line("[catalog] offline-compile-queue consumed executable=" +
                      loaded.executable_path + " pending=" +
                      std::to_string(pending_catalog_compiles.size()));
        }
        return executable_pending || linker_pending;
      };
  const auto precompile_catalog_generation =
      [&output, &record_precompile_outcomes](
          Runtime &runtime, bool pending, std::string_view executable_path) {
        if (!pending) return;
        const auto result = runtime.cpus->precompile_pending(
            maximum_catalog_preexec_blocks,
            catalog_preexec_budget_nanoseconds,
            JitPrecompileTarget::NativeCode);
        record_precompile_outcomes(result);
        output.line("[catalog] exec-precompile executable=" +
                    std::string{executable_path} +
                    " blocks=" + std::to_string(result.native_compiled) +
                    " attempted=" + std::to_string(result.attempted) +
                    " shared-slab=" +
                    std::to_string(result.shared_slab_hits) +
                    " artifact-imported=" +
                    std::to_string(result.artifact_imported) +
                    " deferred=" + std::to_string(result.deferred) +
                    " failed=" + std::to_string(result.failed));
      };
  auto initial = std::make_unique<Runtime>();
  initial->memory = std::move(initial_memory);
  initial->jit_cache_reservation =
      jit_code_cache_governor.reserve(
          guest_processor_count, JitCodeCacheClass::BootCritical);
  initial->jit_cache_class = JitCodeCacheClass::BootCritical;
  if (!initial->jit_cache_reservation) {
    throw std::runtime_error{"failed to reserve initial JIT code cache"};
  }
  initial->cpus = std::make_unique<CpuCluster>(
      initial_guest_thread_slots, maximum_guest_threads, *initial->memory,
      guest_processor_count, *cpu_model, shared_exclusive_monitor,
      allocate_shared_monitor_slots(), jit_artifacts,
      shared_exclusive_address_resolver);
  initial->cpus->set_jit_code_cache_size(
      initial->jit_cache_reservation->shared_slab_bytes());
  output.line(
      "[jit] initial-runtime-shared-slab-mib=" +
      std::to_string(initial->jit_cache_reservation->shared_slab_bytes() /
                     1024U / 1024U));
  assign_jit_process_profile(*initial, process, JitPrecompilePhase::Loader);
  initial->kernel =
      std::make_unique<CompatibilityKernel>(*initial->memory, output, *rootfs,
                                            device, activation_override,
                                            lockdown_profile);
  if (baseband_capture_stream) {
    auto *stream = &*baseband_capture_stream;
    initial->kernel->set_baseband_transmit_sink(
        [stream, &baseband_capture_bytes](std::span<const std::byte> bytes) {
          stream->write(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
          if (!*stream)
            return false;
          baseband_capture_bytes += bytes.size();
          return true;
        });
    output.line("[baseband] capture mode=stream output=" +
                *baseband_output_path);
  } else {
    initial->kernel->set_baseband_capture_enabled(false);
    output.line("[baseband] capture mode=null");
  }
  output.line(std::string{"[baseband] profile="} +
              (baseband_input_path ? "virtual" : "offline") +
              " service=" +
              ((baseband_input_path || device.baseband_device_available)
                   ? "visible"
                   : "unavailable") +
              " mux=" +
              (baseband_input_path ? "enabled" : "disabled") +
              " data=" + (baseband_input_path ? "replay-only" : "none"));
  initial->cpus->set_process_id(initial->kernel->process().pid);
  std::shared_ptr<SdlAudioSink> audio_sink;
  if (SdlAudioSink::available()) {
    audio_sink = std::make_shared<SdlAudioSink>();
    initial->kernel->set_audio_sink(audio_sink);
    output.line("[audio] backend=sdl open=lazy");
  } else {
    output.line("[audio] backend=none");
  }
  if (FfmpegAudioDecoder::available()) {
    initial->kernel->set_audio_decoder(
        std::make_shared<FfmpegAudioDecoder>());
    output.line("[audio] decoder=ffmpeg");
  } else {
    output.line("[audio] decoder=pcm-caf-only");
  }
  initial->kernel->set_process_arguments({binary}, initial_environment);
  initial->kernel->set_process_image(
      process.executable_path,
      process.executable.code_signature_entitlements());
  initial->kernel->enqueue_baseband_input(baseband_input);
  initial->kernel->set_baseband_receive_eof(baseband_input_path.has_value());
  if (baseband_input_path) {
    output.line("[baseband] replay input=" + *baseband_input_path +
                " bytes=" + std::to_string(baseband_input.size()));
  }
  if (sdl_display) {
    initial->kernel->set_display_presenter(
        [backend = sdl_display.get()](DisplayFrame frame) {
          backend->present(std::move(frame));
        });
  } else if (frame_file_presenter) {
    initial->kernel->set_display_presenter(
        [backend = frame_file_presenter.get(),
         &output](DisplayFrame frame) {
          // Animation diagnostics must not turn the measured window into a
          // PNG-writing benchmark. The presenter callback is still the
          // actual CPU-present boundary for the headless sink; retain pixels
          // already carried by the frame for in-memory change detection.
          const auto diagnostic_window =
              performance_counters().frame_content_diagnostics_enabled() &&
              performance_counters().display_window_active();
          const auto file_output_enabled = backend->enabled();
          if (!diagnostic_window && file_output_enabled)
            backend->present(frame);
          performance_counters().record_cpu_present_fallback(
              frame.sequence, frame.submitted_at);
          if (diagnostic_window) {
            const auto pixels = frame.pixels;
            performance_counters().record_diagnostic_frame_content(
                frame.sequence, frame.owner_process_id, frame.submitted_at,
                frame.width, frame.height, pixels);
            return;
          }
          if (!file_output_enabled)
            return;
          const auto pixels =
              !frame.pixels.empty()
                  ? frame.pixels
                  : (frame.read_pixels ? frame.read_pixels()
                                       : std::vector<std::uint32_t>{});
          // A content-diagnostic window is part of the measured presenter
          // path. Do not turn it into a per-frame stdout-flush benchmark:
          // the in-memory content record above already retains the semantic
          // change evidence needed by the offline analyzer.
          if (!diagnostic_window) {
            const auto visible = std::count_if(
                pixels.begin(), pixels.end(), [](std::uint32_t pixel) {
                  return (pixel & 0x00ffffffU) != 0;
                });
            output.line("[display] frame=" +
                        std::to_string(frame.sequence) +
                        " visible-pixels=" + std::to_string(visible));
          }
        });
  }
  initial->allocated.assign(initial_guest_thread_slots, false);
  Runtime *initial_runtime = initial.get();
  std::error_code catalog_root_error;
  const auto catalog_root =
      std::filesystem::absolute(*rootfs, catalog_root_error).lexically_normal();
  HostFileWatcher host_file_watcher{
      catalog_root_error ? std::filesystem::path{*rootfs} : catalog_root};
  output.line(std::string{"[host-watch] enabled="} +
              (host_file_watcher.enabled() ? "true" : "false") +
              " startup-watches=" +
              std::to_string(host_file_watcher.watch_count()));
  bool host_watch_registration_reported =
      !host_file_watcher.registration_pending();
  bool catalog_refresh_pending = false;
  std::vector<std::filesystem::path> catalog_refresh_paths;
  std::map<std::filesystem::path, ExecutableCatalogKnownIdentity>
      catalog_refresh_identities;
  std::uint64_t catalog_refresh_events{};
  std::uint64_t catalog_refresh_count{};
  struct CatalogRefreshCompletion {
    std::uint64_t base_revision{};
    std::vector<std::filesystem::path> paths;
    std::map<std::filesystem::path, ExecutableCatalogKnownIdentity>
        known_identities;
    std::shared_ptr<ExecutableCatalog> catalog;
    ExecutableCatalogScanSummary summary;
    std::filesystem::path staged_manifest;
    bool manifest_staged{};
    std::string error;
  };
  struct CatalogRefreshState {
    std::mutex mutex;
    std::optional<CatalogRefreshCompletion> completion;
  };
  const auto catalog_refresh_state =
      std::make_shared<CatalogRefreshState>();
  std::shared_ptr<HostWorkToken> catalog_refresh_task;
  auto next_catalog_refresh_submission =
      HostResourceController::Clock::time_point{};
  std::uint64_t catalog_refresh_sequence{};
  std::uint64_t catalog_refresh_scheduled{};
  std::uint64_t catalog_refresh_rejected{};
  std::uint64_t catalog_refresh_stale{};
  const auto refresh_catalog_after_file_mutations =
      [&](bool refresh_when_idle, bool allow_schedule = true) {
        constexpr std::size_t maximum_mutations_per_poll = 128;
        if (refresh_when_idle &&
            initial_runtime->kernel->display_submitted_frames() != 0U) {
          host_file_watcher.advance_registration(256);
          if (!host_watch_registration_reported &&
              !host_file_watcher.registration_pending()) {
            output.line("[host-watch] registration=complete watches=" +
                        std::to_string(host_file_watcher.watch_count()));
            host_watch_registration_reported = true;
          }
        }
        const auto queue_catalog_path =
            [&](const std::filesystem::path &path,
                std::optional<ExecutableCatalogKnownIdentity> known_identity =
                    std::nullopt) {
              if (std::find(catalog_refresh_paths.begin(),
                            catalog_refresh_paths.end(), path) ==
                  catalog_refresh_paths.end()) {
                catalog_refresh_paths.push_back(path);
              }
              if (known_identity) {
                catalog_refresh_identities[path] = std::move(*known_identity);
              }
            };
        std::optional<CatalogRefreshCompletion> catalog_completion;
        {
          const std::lock_guard lock{catalog_refresh_state->mutex};
          if (catalog_refresh_state->completion) {
            catalog_completion =
                std::move(catalog_refresh_state->completion);
            catalog_refresh_state->completion.reset();
          }
        }
        if (catalog_completion) {
          catalog_refresh_task.reset();
          const auto requeue_completion_paths = [&] {
            for (const auto &path : catalog_completion->paths) {
              if (const auto known =
                      catalog_completion->known_identities.find(path);
                  known != catalog_completion->known_identities.end()) {
                queue_catalog_path(path, known->second);
              } else {
                queue_catalog_path(path);
              }
            }
            catalog_refresh_pending = !catalog_refresh_paths.empty();
          };
          if (!catalog_completion->error.empty()) {
            requeue_completion_paths();
            output.line("[catalog] mutation-refresh failed error=" +
                        catalog_completion->error);
          } else if (executable_catalog.revision() !=
                     catalog_completion->base_revision) {
            ++catalog_refresh_stale;
            requeue_completion_paths();
            std::error_code remove_error;
            std::filesystem::remove(catalog_completion->staged_manifest,
                                    remove_error);
          } else {
            std::unordered_set<ContentIdentity, ContentIdentityHash> previous;
            if (catalog_index != nullptr) {
              for (const auto &identity :
                   executable_catalog.content_identities()) {
                previous.insert(identity);
              }
            }
            executable_catalog = std::move(*catalog_completion->catalog);
            catalog_loaded = true;
            catalog_index = &executable_catalog;
            for (const auto &identity :
                 executable_catalog.content_identities()) {
              if (previous.contains(identity) ||
                  std::find(pending_catalog_compiles.begin(),
                            pending_catalog_compiles.end(), identity) !=
                      pending_catalog_compiles.end()) {
                continue;
              }
              if (pending_catalog_compiles.size() >=
                  maximum_catalog_offline_compile_queue) {
                pending_catalog_compiles.pop_front();
              }
              pending_catalog_compiles.push_back(identity);
            }
            bool manifest_published = false;
            if (catalog_completion->manifest_staged) {
              std::error_code rename_error;
              std::filesystem::rename(catalog_completion->staged_manifest,
                                      catalog_manifest, rename_error);
              manifest_published = !rename_error;
              if (rename_error) {
                std::error_code remove_error;
                std::filesystem::remove(
                    catalog_completion->staged_manifest, remove_error);
              }
            }
            if (!manifest_published) {
              output.line("[catalog] mutation-refresh manifest-save=failed");
            }
            output.line(
                "[catalog] mutation-refresh regular-files=" +
                std::to_string(
                    catalog_completion->summary.regular_files) +
                " macho-images=" +
                std::to_string(catalog_completion->summary.mach_o_images) +
                " failed-files=" +
                std::to_string(catalog_completion->summary.failed_files) +
                " paths=" +
                std::to_string(catalog_completion->paths.size()) +
                " new-offline-queue=" +
                std::to_string(pending_catalog_compiles.size()) +
                " async=true");
            ++catalog_refresh_count;
          }
        }
        if (!allow_schedule) return;
        host_file_watcher.poll();
        const auto host_changes = host_file_watcher.publish_stable(
            host_resources,
            *initial_runtime->kernel->guest_file_generation_registry(), 64,
            refresh_when_idle);
        for (const auto &path : host_changes.changed_paths) {
          if (const auto known = host_changes.stable_identities.find(path);
              known != host_changes.stable_identities.end()) {
            const auto &generation = known->second.generation;
            queue_catalog_path(
                path,
                ExecutableCatalogKnownIdentity{
                    ExecutableCatalogFileGeneration{
                        generation.device, generation.inode,
                        generation.file_size, generation.modified_seconds,
                        generation.modified_nanoseconds,
                        generation.changed_seconds,
                        generation.changed_nanoseconds},
                    known->second.content_identity});
          } else {
            queue_catalog_path(path);
          }
          catalog_refresh_pending = true;
        }
        bool structural_boundary = !host_changes.changed_paths.empty() ||
                                   !host_changes.structural_events.empty() ||
                                   !host_changes.dirty_subtrees.empty();
        for (const auto &event : host_changes.structural_events) {
          // Directory structure events are authoritative subtree boundaries.
          // They must reach the catalog even when the directory did not
          // previously contain a known executable.
          queue_catalog_path(event.path);
          catalog_refresh_pending = true;
        }
        for (const auto &subtree : host_changes.dirty_subtrees) {
          queue_catalog_path(subtree);
          catalog_refresh_pending = true;
        }
        const auto mutations = initial_runtime->kernel->take_guest_file_mutations(
            maximum_mutations_per_poll);
        for (const auto &mutation : mutations) {
          ++catalog_refresh_events;
          if (mutation.dirty_subtree) {
            // The registry deliberately coalesces an overflow into a
            // structural marker.  Individual paths before this marker were
            // evicted, so refresh the catalog root instead of pretending the
            // remaining event list is complete.
            if (!catalog_root_error) {
              queue_catalog_path(catalog_root);
              catalog_refresh_pending = true;
              structural_boundary = true;
            }
            continue;
          }
          const auto relative = mutation.path.lexically_relative(catalog_root);
          if (catalog_root_error || relative.empty() || relative == "." ||
              relative.begin() == relative.end() ||
              *relative.begin() == "..") {
            continue;
          }
          const auto guest_path =
              "/" + relative.generic_string();
          const auto application_path =
              is_application_executable_path(guest_path);
          const auto known_executable =
              catalog_index != nullptr &&
              catalog_index->find_path(mutation.path) != nullptr;
          switch (mutation.mutation) {
          case GuestFileMutationKind::SubtreeCreate:
          case GuestFileMutationKind::SubtreeRemove:
            // Namespace changes are already subtree-scoped.  Do not consult
            // the old catalog index: this is how a newly installed bundle
            // becomes visible to the next exec/mmap refresh.
            queue_catalog_path(mutation.path);
            catalog_refresh_pending = true;
            structural_boundary = true;
            break;
          case GuestFileMutationKind::InstallReplace:
          case GuestFileMutationKind::Rename:
          case GuestFileMutationKind::Unlink:
            if (known_executable || application_path || catalog_index == nullptr) {
              queue_catalog_path(mutation.path);
              catalog_refresh_pending = true;
              structural_boundary = true;
            }
            break;
          case GuestFileMutationKind::Truncate:
          case GuestFileMutationKind::Write:
          case GuestFileMutationKind::SharedWriteback:
            if (known_executable || application_path) {
              queue_catalog_path(mutation.path);
              catalog_refresh_pending = true;
            }
            break;
          case GuestFileMutationKind::Observation:
            break;
          }
        }
        if (!catalog_refresh_pending || catalog_refresh_paths.empty() ||
            (!refresh_when_idle && !structural_boundary)) {
          return;
        }

        if (catalog_refresh_task ||
            HostResourceController::Clock::now() <
                next_catalog_refresh_submission) {
          return;
        }
        auto refresh_paths = std::make_shared<
            std::vector<std::filesystem::path>>(
            std::move(catalog_refresh_paths));
        catalog_refresh_paths.clear();
        auto refresh_identities = std::make_shared<
            std::map<std::filesystem::path, ExecutableCatalogKnownIdentity>>(
                std::move(catalog_refresh_identities));
        catalog_refresh_identities.clear();
        catalog_refresh_pending = false;
        auto catalog_snapshot =
            std::make_shared<ExecutableCatalog>(executable_catalog);
        const auto base_revision = executable_catalog.revision();
        const auto sequence = ++catalog_refresh_sequence;
        const auto staged_manifest = std::filesystem::path{
            catalog_manifest + ".refresh-" + std::to_string(sequence)};
        catalog_refresh_task = host_resources.submit(
            HostWorkKind::Maintenance, std::nullopt,
            [state = catalog_refresh_state,
             catalog = std::move(catalog_snapshot),
             paths = refresh_paths, root = *rootfs,
             architecture = guest_architecture, base_revision,
             staged_manifest, known_identities = refresh_identities]() mutable {
              CatalogRefreshCompletion completion;
              completion.base_revision = base_revision;
              completion.paths = std::move(*paths);
              completion.known_identities = *known_identities;
              completion.catalog = std::move(catalog);
              completion.staged_manifest = staged_manifest;
              try {
                completion.summary = completion.catalog->refresh_paths(
                    root, completion.paths, architecture, *known_identities);
                completion.manifest_staged =
                    completion.catalog->save(staged_manifest);
              } catch (const std::exception &error) {
                completion.error = error.what();
              } catch (...) {
                completion.error = "unknown background refresh failure";
              }
              const std::lock_guard lock{state->mutex};
              state->completion = std::move(completion);
            },
            std::chrono::milliseconds{50});
        if (catalog_refresh_task) {
          ++catalog_refresh_scheduled;
        } else {
          ++catalog_refresh_rejected;
          next_catalog_refresh_submission =
              HostResourceController::Clock::now() +
              std::chrono::milliseconds{50};
          for (const auto &path : *refresh_paths) {
            if (const auto known = refresh_identities->find(path);
                known != refresh_identities->end()) {
              queue_catalog_path(path, known->second);
            } else {
              queue_catalog_path(path);
            }
          }
          catalog_refresh_pending = !catalog_refresh_paths.empty();
        }
      };
  runtimes.push_back(std::move(initial));
  runtime_index.insert(*initial_runtime);
  initial_runtime->kernel->set_preferred_wifi_networks(
      preferred_wifi_networks);
  BootGdbTarget debug_target{runtimes};
  XnuScheduler scheduler{
      guest_ticks_per_second /
          xnu792::scheduler::default_preemption_rate,
      guest_ticks_per_second /
          xnu792::scheduler::scheduler_ticks_per_second,
      guest_processor_count};
  GuestParallelismPolicy guest_parallelism_policy{guest_ticks_per_second};
  std::optional<XnuThreadId> last_serial_thread;
  std::optional<std::uint32_t> display_urgent_process;
  std::optional<XnuThreadId> display_urgent_thread;

  std::uint32_t next_pid = 2;
  std::size_t watchpoint_trace_count = 0;
  std::mutex watchpoint_mutex;
  std::uint64_t catalog_mapped_executable_ranges = 0;
  std::uint64_t catalog_mapped_entry_hints = 0;
  std::array<std::uint64_t, jit_precompile_phase_count>
      catalog_mapped_entry_hints_by_phase{};
  std::array<std::uint64_t, jit_precompile_phase_count>
      precompile_tasks_by_phase{};
  std::array<std::atomic<std::uint64_t>, jit_precompile_phase_count>
      precompile_blocks_by_phase{};
  std::array<std::uint64_t, jit_precompile_target_count>
      precompile_tasks_by_target{};
  std::array<std::atomic<std::uint64_t>, jit_precompile_target_count>
      precompile_blocks_by_target{};
  std::function<void(Runtime &)> configure_runtime;
  configure_runtime = [&](Runtime &runtime) {
    auto *runtime_ptr = &runtime;
    runtime.kernel->set_host_network_policy(*network_policy);
    runtime.kernel->set_mapped_executable_handler(
        [runtime_ptr, &catalog_index, &catalog_mapped_executable_ranges,
         &catalog_mapped_entry_hints, &catalog_mapped_entry_hints_by_phase](
            const std::filesystem::path &path, std::uint32_t mapping_address,
            std::uint32_t mapping_size, std::uint64_t file_offset) {
          if (catalog_index == nullptr) return;
          auto entry_points = catalog_index->fixed_mapping_entry_points(
              path, mapping_address, mapping_size, file_offset);
          if (entry_points.empty()) return;
          ++catalog_mapped_executable_ranges;
          catalog_mapped_entry_hints += entry_points.size();
          catalog_mapped_entry_hints_by_phase[static_cast<std::size_t>(
              runtime_ptr->precompile_phase)] += entry_points.size();
          runtime_ptr->cpus->add_precompile_entries(
              entry_points, runtime_ptr->precompile_phase);
        });
    if (!runtime.kernel->set_virtual_processor_count(guest_processor_count)) {
      throw std::runtime_error{"invalid virtual processor topology"};
    }
    const auto configure_cpu = [&, runtime_ptr](std::size_t index) {
      auto &cpu = runtime.cpus->cpu(index);
      runtime.kernel->attach(cpu);
      cpu.set_svc_dispatch_mode(guest_processor_count > 1
                                    ? SvcDispatchMode::Deferred
                                    : SvcDispatchMode::Immediate);
      cpu.set_debug_breakpoints_enabled(gdb_port.has_value());
      if (watch_address) {
        cpu.set_memory_write_watchpoint(
            *watch_address,
            [&, runtime_ptr](Cpu &source, std::uint32_t address,
                             std::size_t size, std::uint64_t value) {
              const std::scoped_lock lock{watchpoint_mutex};
              if (watchpoint_trace_count >= maximum_watchpoint_traces)
                return;
              ++watchpoint_trace_count;
              std::ostringstream message;
              message << "[watch] pid=" << runtime_ptr->kernel->process().pid
                      << " cpu=" << source.processor_id() << " pc=0x"
                      << std::hex << source.registers()[15] << " address=0x"
                      << address << " size=0x" << size << " value=0x" << value;
              for (std::size_t register_index = 0; register_index < 4;
                   ++register_index) {
                message << " r" << std::dec << register_index << "=0x"
                        << std::hex << source.registers()[register_index];
              }
              message << " sp=0x" << source.registers()[13] << " lr=0x"
                      << source.registers()[14];
              output.line(message.str());
            });
      }
    };
    for (std::size_t index = 0; index < runtime.cpus->size(); ++index) {
      configure_cpu(index);
    }
    runtime.kernel->set_thread_create_handler(
        [runtime_ptr, &scheduler,
         configure_cpu](const std::array<std::uint32_t, 16> &registers,
                        std::uint32_t cpsr) -> std::optional<std::size_t> {
          const auto allocate_slot =
              [&](std::size_t index) -> std::optional<std::size_t> {
            if (runtime_ptr->allocated[index])
              return std::nullopt;
            auto &child = runtime_ptr->cpus->cpu(index);
            child.reset();
            child.registers() = registers;
            child.set_cpsr(cpsr);
            runtime_ptr->allocated[index] = true;
            const auto registered = scheduler.register_thread(
                XnuThreadId{runtime_ptr->kernel->process().pid,
                            static_cast<std::uint32_t>(index)},
                runtime_ptr->kernel->process().thread_base_priority);
            if (!registered) {
              runtime_ptr->allocated[index] = false;
              return std::nullopt;
            }
            return index;
          };
          for (std::size_t index = 1; index < runtime_ptr->cpus->size();
               ++index) {
            if (runtime_ptr->allocated[index])
              continue;
            return allocate_slot(index);
          }
          const auto added = runtime_ptr->cpus->add_cpu();
          if (!added)
            return std::nullopt;
          runtime_ptr->allocated.push_back(false);
          configure_cpu(*added);
          return allocate_slot(*added);
        });
    runtime.kernel->set_thread_terminate_handler(
        [runtime_ptr, &scheduler,
         &guest_parallelism_policy](std::uint32_t pid,
                                    std::size_t processor) {
          if (pid != runtime_ptr->kernel->process().pid ||
              processor >= runtime_ptr->allocated.size() ||
              !runtime_ptr->allocated[processor] ||
              !scheduler.remove_thread(
                  XnuThreadId{pid, static_cast<std::uint32_t>(processor)})) {
            return false;
          }
          runtime_ptr->kernel->clear_thread_io_policy(processor);
          guest_parallelism_policy.forget(
              XnuThreadId{pid, static_cast<std::uint32_t>(processor)});
          runtime_ptr->allocated[processor] = false;
          return true;
        });
    runtime.kernel->set_thread_state_query(
        [&runtime_index](std::uint32_t pid, std::uint32_t slot,
                         std::uint32_t flavor)
            -> std::optional<darwin::arm_thread::GeneralState> {
          if (flavor != darwin::arm_thread::general_state_flavor) {
            return std::nullopt;
          }
          const auto *runtime = runtime_index.find(pid);
          if (runtime == nullptr || slot >= runtime->cpus->size() ||
              slot >= runtime->allocated.size() || !runtime->allocated[slot]) {
            return std::nullopt;
          }
          const auto &thread = runtime->cpus->cpu(slot);
          darwin::arm_thread::GeneralState state{};
          std::copy(thread.registers().begin(), thread.registers().end(),
                    state.begin());
          state[darwin::arm_thread::cpsr_index] = thread.cpsr();
          return state;
        });
    runtime.kernel->set_thread_state_update_handler(
        [&runtime_index](std::uint32_t pid, std::uint32_t slot,
                         const darwin::arm_thread::GeneralState &state) {
          const auto *runtime = runtime_index.find(pid);
          if (runtime == nullptr || slot >= runtime->cpus->size() ||
              slot >= runtime->allocated.size() || !runtime->allocated[slot]) {
            return false;
          }
          auto &thread = runtime->cpus->cpu(slot);
          std::copy_n(state.begin(), thread.registers().size(),
                      thread.registers().begin());
          thread.set_cpsr(state[darwin::arm_thread::cpsr_index] | 0x10U);
          return true;
        });
    runtime.kernel->set_thread_pointer_update_handler(
        [&runtime_index](std::uint32_t pid, std::uint32_t slot,
                         std::optional<std::uint32_t> cthread_self) {
          const auto *runtime = runtime_index.find(pid);
          if (runtime == nullptr || slot >= runtime->cpus->size() ||
              slot >= runtime->allocated.size() || !runtime->allocated[slot]) {
            return false;
          }
          runtime->cpus->cpu(slot).set_cthread_self(cthread_self);
          return true;
        });
    runtime.kernel->set_thread_runnable_handler(
        [&scheduler](std::uint32_t pid, std::uint32_t slot, bool runnable) {
          const XnuThreadId thread{pid, slot};
          return runnable ? scheduler.resume_thread(thread)
                          : scheduler.suspend_thread(thread);
        });
    runtime.kernel->set_thread_wake_handler(
        [&scheduler](std::uint32_t pid, std::uint32_t slot) {
          return scheduler.wake_thread(XnuThreadId{pid, slot});
        });
    runtime.kernel->set_mach_message_wake_handler(
        [&runtime_index, &scheduler](std::uint32_t pid,
                                     std::uint32_t object) {
          auto *receiver = runtime_index.find(pid);
          if (receiver == nullptr)
            return XnuThreadWakeResult{};
          const auto processor =
              receiver->kernel->pending_mach_receiver_processor(object);
          if (!processor)
            return XnuThreadWakeResult{};
          return scheduler.wake_thread(
              XnuThreadId{pid, static_cast<std::uint32_t>(*processor)});
        });
    const auto create_child_runtime =
        [&, runtime_ptr](Cpu *parent_cpu,
                         CompatibilityKernel::ProcessInheritance inheritance)
        -> std::optional<std::uint32_t> {
          const auto child_pid = next_pid++;
          auto child = std::make_unique<Runtime>();
          const auto child_cache_class =
              inheritance == CompatibilityKernel::ProcessInheritance::Fork
                  ? runtime_ptr->jit_cache_class
                  : JitCodeCacheClass::Foreground;
          child->jit_cache_reservation =
              jit_code_cache_governor.reserve(guest_processor_count,
                                               child_cache_class);
          if (!child->jit_cache_reservation) {
            return std::nullopt;
          }
          child->jit_cache_class = child_cache_class;
          if (inheritance ==
              CompatibilityKernel::ProcessInheritance::SpawnExec) {
            PerformanceLatencyScope latency{
                PerfLatencyKind::ProcessFreshMemory};
            child->memory = std::make_unique<AddressSpace>();
            child->memory->set_parallel_access(guest_processor_count > 1);
            child->fresh_spawn_address_space = true;
          } else {
            PerformanceLatencyScope latency{
                PerfLatencyKind::ProcessCloneMemory};
            child->memory = runtime_ptr->memory->clone();
            debug_target.prepare_fork_child(
                runtime_ptr->kernel->process().pid, *child->memory);
          }
          {
            PerformanceLatencyScope latency{PerfLatencyKind::ProcessCreateCpu};
            child->cpus = std::make_unique<CpuCluster>(
                initial_guest_thread_slots, maximum_guest_threads,
                *child->memory, guest_processor_count, *cpu_model,
                shared_exclusive_monitor, allocate_shared_monitor_slots(),
                jit_artifacts, shared_exclusive_address_resolver);
            child->cpus->set_jit_code_cache_size(
                child->jit_cache_reservation->shared_slab_bytes());
          }
          {
            PerformanceLatencyScope latency{
                PerfLatencyKind::ProcessCreateKernel};
            child->kernel = std::make_unique<CompatibilityKernel>(
                *child->memory, output, *rootfs, device, activation_override,
                lockdown_profile);
          }
          if (inheritance ==
              CompatibilityKernel::ProcessInheritance::SpawnExec) {
            PerformanceLatencyScope latency{
                PerfLatencyKind::ProcessInheritSpawnKernel};
            child->kernel->inherit_process_state(*runtime_ptr->kernel,
                                                 child_pid, inheritance);
          } else {
            PerformanceLatencyScope latency{
                PerfLatencyKind::ProcessInheritKernel};
            child->kernel->inherit_process_state(*runtime_ptr->kernel,
                                                 child_pid, inheritance);
          }
          child->cpus->set_process_id(child_pid);
          child->allocated.assign(initial_guest_thread_slots, false);
          {
            PerformanceLatencyScope latency{
                PerfLatencyKind::ProcessConfigureRuntime};
            configure_runtime(*child);
          }
          if (parent_cpu != nullptr) {
            auto &child_cpu = child->cpus->cpu(0);
            child_cpu.registers() = parent_cpu->registers();
            child_cpu.extension_registers() =
                parent_cpu->extension_registers();
            child_cpu.registers()[0] = 0;
            child_cpu.set_cpsr(parent_cpu->cpsr() & ~(1U << 29U));
            child_cpu.set_fpscr(parent_cpu->fpscr());
            child_cpu.set_cthread_self(parent_cpu->cthread_self());
          }
          child->allocated[0] = true;
          static_cast<void>(scheduler.register_thread(
              XnuThreadId{child_pid, 0},
              child->kernel->process().thread_base_priority));
          runtime_index.insert(*child);
          runtimes.push_back(std::move(child));
          return child_pid;
        };
    runtime.kernel->set_fork_handler(
        [create_child_runtime](Cpu &parent_cpu) {
          return create_child_runtime(
              &parent_cpu, CompatibilityKernel::ProcessInheritance::Fork);
        });
    runtime.kernel->set_spawn_create_handler(
        [create_child_runtime](Cpu &) {
          return create_child_runtime(
              nullptr,
              CompatibilityKernel::ProcessInheritance::SpawnExec);
        });
    runtime.kernel->set_exec_handler(
        [&, runtime_ptr](Cpu &source, std::string path,
                         std::vector<std::string> arguments,
                         std::vector<std::string> environment) {
          refresh_catalog_after_file_mutations(true);
          ProcessLoader validator{*rootfs, *runtime_ptr->memory,
                                  guest_architecture, catalog_index};
          if (!validator.validate(path)) {
            output.line("[process] exec rejected pid=" +
                        std::to_string(runtime_ptr->kernel->process().pid) +
                        " path=" + path);
            return false;
          }
          runtime_ptr->pending_exec = PendingExec{
              source.processor_id(),
              std::move(path),
              std::move(arguments),
              std::move(environment),
          };
          return true;
        });
    runtime.kernel->set_spawn_exec_handler(
        [&](std::uint32_t child_pid, std::string path,
            std::vector<std::string> arguments,
            std::vector<std::string> environment, bool start_suspended) {
          auto *child_runtime = runtime_index.find(child_pid);
          if (child_runtime == nullptr)
            return false;

          const auto image_epoch =
              child_runtime->begin_image_transition(host_resources);
          try {
            refresh_catalog_after_file_mutations(true);
            debug_target.notify_exec(child_pid);
            if (!child_runtime->fresh_spawn_address_space) {
              PerformanceLatencyScope latency{
                  PerfLatencyKind::SpawnMemoryClear};
              child_runtime->memory->clear();
            }
            LoadedProcess loaded;
            {
              PerformanceLatencyScope latency{PerfLatencyKind::SpawnImageLoad};
              ProcessLoader loader{*rootfs, *child_runtime->memory,
                                   guest_architecture, catalog_index};
              loaded = loader.load(path, std::move(arguments), environment);
            }
            {
              PerformanceLatencyScope latency{
                  PerfLatencyKind::SpawnResetRuntime};
              child_runtime->kernel->set_process_arguments(loaded.arguments,
                                                           environment);
              child_runtime->kernel->set_process_image(
                  path, loaded.executable.code_signature_entitlements());
              const auto catalog_generation_pending =
                  assign_jit_process_profile(
                      *child_runtime, loaded,
                      precompile_phase_for_process(loaded.executable_path));
              child_runtime->kernel->prepare_exec(0);
              auto &child_cpu = child_runtime->cpus->cpu(0);
              child_cpu.reset();
              child_cpu.clear_cache();
              child_cpu.registers().fill(0);
              child_cpu.registers()[13] = loaded.stack_pointer;
              child_cpu.registers()[15] = loaded.entry_point;
              child_cpu.set_cpsr(0x10);
              child_runtime->kernel->install_main_image_hle(
                  child_cpu, loaded.executable_path);
              precompile_catalog_generation(
                  *child_runtime, catalog_generation_pending,
                  loaded.executable_path);
            }
            child_runtime->activate_image_epoch(image_epoch);
            child_runtime->fresh_spawn_address_space = false;
            if (start_suspended) {
              static_cast<void>(scheduler.block(XnuThreadId{child_pid, 0}));
            }
          } catch (const std::exception &error) {
            output.line("[process] spawn exec failed pid=" +
                        std::to_string(child_pid) + " path=" + path +
                        " error=" + error.what());
            child_runtime->kernel->exit_process(127);
            scheduler.remove_process(child_pid);
            guest_parallelism_policy.forget_process(child_pid);
            std::fill(child_runtime->allocated.begin(),
                      child_runtime->allocated.end(), false);
            return false;
          }
          return true;
        });
    runtime.kernel->set_scheduler_runnable_query(
        [&scheduler, runtime_ptr](std::size_t thread_slot) {
          return scheduler.should_yield(XnuThreadId{
              runtime_ptr->kernel->process().pid,
              static_cast<std::uint32_t>(thread_slot)});
        });
    runtime.kernel->set_signal_delivery_handler(
        [&runtime_index, &scheduler,
         &guest_parallelism_policy](std::uint32_t target_pid,
                                    std::uint32_t signal) {
          auto *target = runtime_index.find(target_pid);
          if (target == nullptr)
            return darwin::error::no_such_process;
          const auto error = target->kernel->deliver_signal(signal);
          if (error == 0 && target->kernel->process().exited) {
            scheduler.remove_process(target_pid);
            guest_parallelism_policy.forget_process(target_pid);
          }
          return error;
        });
    runtime.kernel->set_task_memory_region_query(
        [&runtime_index](std::uint32_t pid, std::uint32_t address)
            -> std::optional<AddressSpace::MappingRegion> {
          const auto *runtime = runtime_index.find(pid);
          if (runtime == nullptr)
            return std::nullopt;
          return runtime->memory->mapping_region_at_or_after(address);
        });
    runtime.kernel->set_task_memory_share_query(
        [&runtime_index](std::uint32_t pid, std::uint32_t address,
                         std::uint32_t size)
            -> std::optional<CompatibilityKernel::SharedTaskMemoryRange> {
          const auto *runtime = runtime_index.find(pid);
          if (runtime == nullptr)
            return std::nullopt;
          const auto region =
              runtime->memory->mapping_region_at_or_after(address);
          const auto end = static_cast<std::uint64_t>(address) + size;
          if (!region || region->address > address || region->end < end)
            return std::nullopt;
          auto pages = runtime->memory->share_pages(address, size);
          if (!pages)
            return std::nullopt;
          return CompatibilityKernel::SharedTaskMemoryRange{
              std::move(*pages), region->permissions};
        });
    runtime.kernel->set_scheduler_preemption_query(
        [runtime_ptr, &scheduler](std::size_t processor) {
          const XnuThreadId thread{runtime_ptr->kernel->process().pid,
                                   static_cast<std::uint32_t>(processor)};
          const auto scheduling_info = scheduler.info(thread);
          return scheduling_info && scheduling_info->last_processor &&
                 scheduler.preemption_for(thread,
                                          *scheduling_info->last_processor) !=
                     XnuPreemption::None;
        });
    runtime.kernel->set_task_priority_handler(
        [runtime_ptr, &scheduler](std::int32_t priority) {
          for (std::size_t processor = 0;
               processor < runtime_ptr->allocated.size(); ++processor) {
            if (!runtime_ptr->allocated[processor])
              continue;
            static_cast<void>(scheduler.set_base_priority(
                XnuThreadId{runtime_ptr->kernel->process().pid,
                            static_cast<std::uint32_t>(processor)},
                priority));
          }
        });
    runtime.kernel->set_legacy_thread_policy_handler(
        [runtime_ptr, &scheduler](std::size_t processor, std::uint32_t policy,
                                  std::int32_t base_priority, bool) {
          using namespace darwin::mach::thread_policy;
          const XnuThreadId thread{runtime_ptr->kernel->process().pid,
                                   static_cast<std::uint32_t>(processor)};
          const auto timeshare = policy == legacy_timeshare_policy;
          if (!timeshare && policy != legacy_round_robin_policy &&
              policy != legacy_fifo_policy) {
            return false;
          }
          return scheduler.set_timeshare(thread, timeshare) &&
                 scheduler.set_base_priority(thread, base_priority);
        });
    runtime.kernel->set_thread_policy_handler(
        [runtime_ptr, &scheduler,
         guest_ticks_per_second](std::size_t processor, std::uint32_t flavor,
                                 std::span<const std::uint32_t> policy) {
          using namespace darwin::mach::thread_policy;
          const XnuThreadId thread{runtime_ptr->kernel->process().pid,
                                   static_cast<std::uint32_t>(processor)};
          if (flavor == extended_policy &&
              policy.size() >= extended_policy_word_count) {
            return scheduler.set_timeshare(thread, policy[0] != 0);
          }
          if (flavor == time_constraint_policy &&
              policy.size() >= time_constraint_policy_word_count) {
            const auto to_scheduler_ticks =
                [guest_ticks_per_second](std::uint32_t value) {
              return duration_to_guest_ticks(
                  value, absolute_time_units_per_second,
                  guest_ticks_per_second);
            };
            return scheduler.set_realtime(
                thread, to_scheduler_ticks(policy[realtime_period_index]),
                to_scheduler_ticks(policy[realtime_computation_index]),
                to_scheduler_ticks(policy[realtime_constraint_index]),
                policy[realtime_preemptible_index] != 0);
          }
          if (flavor == precedence_policy &&
              policy.size() >= precedence_policy_word_count) {
            const auto importance = std::bit_cast<std::int32_t>(
                policy[precedence_importance_index]);
            return scheduler.set_base_priority(
                thread, runtime_ptr->kernel->process().thread_base_priority +
                            importance);
          }
          return false;
        });
  };
  configure_runtime(*initial_runtime);

  auto &initial_cpu = initial_runtime->cpus->cpu(0);
  initial_runtime->allocated[0] = true;
  static_cast<void>(scheduler.register_thread(
      XnuThreadId{initial_runtime->kernel->process().pid, 0},
      initial_runtime->kernel->process().thread_base_priority));
  initial_cpu.registers()[13] = process.stack_pointer;
  initial_cpu.registers()[15] = process.entry_point;
  initial_cpu.set_cpsr(0x10);

  {
    std::ostringstream message;
    message << "[loader] main=0x" << std::hex << process.main_header
            << " dyld_entry=0x" << process.entry_point << " sp=0x"
            << process.stack_pointer << std::dec
            << " processors=" << guest_processor_count
            << " network=" << host_network_policy_name(*network_policy) << '\n';
    output.write(message.str());
  }
  std::uint64_t remaining_ticks = ticks;
  std::uint64_t consumed_ticks = 0;
  std::uint32_t stopped_pid = 1;
  std::size_t stopped_cpu = 0;
  CpuRunResult stopped_result{};
  bool hard_stop = false;
  std::unique_ptr<GdbRemoteServer> gdb_server;
  std::optional<GdbResumeRequest> debug_request;
  if (gdb_port) {
    gdb_server = std::make_unique<GdbRemoteServer>(*gdb_port, output);
    gdb_server->listen_and_accept();
    const GdbThreadId initial_thread{1, 1};
    debug_target.set_current_thread(initial_thread);
    auto request = gdb_server->command_loop(debug_target, initial_thread);
    if (request.kind == GdbResumeKind::Detach) {
      debug_target.remove_all_breakpoints();
      gdb_server->detach();
      gdb_server.reset();
      for (auto &runtime : runtimes) {
        for (std::size_t processor = 0; processor < runtime->cpus->size();
             ++processor) {
          runtime->cpus->cpu(processor).set_debug_breakpoints_enabled(false);
        }
      }
    } else if (request.kind == GdbResumeKind::Kill) {
      hard_stop = true;
    } else {
      debug_request = request;
    }
  }
  if (touch_replay) {
    touch_replay->start();
  }
  std::optional<RealtimePacer> realtime_pacer;
  std::vector<std::pair<std::chrono::steady_clock::time_point,
                        std::filesystem::path>>
      scheduled_snapshots;
  enum class HostDeadlineSource : std::uint8_t {
    TouchReplay,
    LiveButton,
    LiveTouch,
    Snapshot,
  };
  DeadlineQueue<HostDeadlineSource, std::chrono::steady_clock::time_point>
      host_deadlines;
  const auto refresh_host_deadlines = [&]() {
    if (touch_replay) {
      if (const auto deadline = touch_replay->next_deadline())
        host_deadlines.upsert(HostDeadlineSource::TouchReplay, *deadline);
      else
        host_deadlines.erase(HostDeadlineSource::TouchReplay);
    } else {
      host_deadlines.erase(HostDeadlineSource::TouchReplay);
    }
    if (const auto deadline = live_button_scheduler.next_deadline())
      host_deadlines.upsert(HostDeadlineSource::LiveButton, *deadline);
    else
      host_deadlines.erase(HostDeadlineSource::LiveButton);
    if (const auto deadline = live_touch_scheduler.next_deadline())
      host_deadlines.upsert(HostDeadlineSource::LiveTouch, *deadline);
    else
      host_deadlines.erase(HostDeadlineSource::LiveTouch);
    if (!scheduled_snapshots.empty())
      host_deadlines.upsert(HostDeadlineSource::Snapshot,
                            scheduled_snapshots.front().first);
    else
      host_deadlines.erase(HostDeadlineSource::Snapshot);
  };
  const auto next_host_control_deadline = [&]() {
    refresh_host_deadlines();
    return host_deadlines.next_deadline();
  };
  const auto wait_for_host_activity = [&](std::chrono::nanoseconds delay) {
    if (delay <= std::chrono::nanoseconds::zero()) return;
    const auto waitable_sdl_session =
        sdl_display && (!live_control || live_control->closed()) &&
        !gdb_server;
    if (waitable_sdl_session) {
      static_cast<void>(sdl_display->wait_for_event(delay));
    } else if (sdl_display) {
      delay = std::min(
          delay,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              sdl_event_poll_fallback));
    }
    if (live_control && !live_control->closed()) {
      live_control->wait_for(delay);
    } else {
      std::this_thread::sleep_for(delay);
    }
  };
  std::optional<std::string> display_performance_window;
  struct DisplayClockWindow {
    std::chrono::steady_clock::time_point started_at;
    std::uint64_t guest_started_at{};
    std::uint64_t pacer_started_at{};
    std::uint64_t rebase_count{};
    std::uint64_t rebase_deficit_total_nanoseconds{};
    std::uint64_t rebase_deficit_max_nanoseconds{};
  };
  std::optional<DisplayClockWindow> display_clock_window;
  Runtime *display_scanout_owner = nullptr;
  auto observed_display_submissions =
      initial_runtime->kernel->display_submitted_frames();
  auto last_display_submission = std::chrono::steady_clock::now();
  auto last_jit_quota_refresh = last_display_submission;
  auto latest_host_memory_budget = jit_cache_budget.memory;
  bool pressure_reclamation_applied{};
  std::optional<std::chrono::steady_clock::time_point> guest_idle_since;
  DeadlineQueue<std::uint32_t, std::uint64_t> guest_deadlines;
  if (!bounded_execution) {
    realtime_pacer.emplace(initial_runtime->kernel->current_absolute_time());
    const auto host_wall_time =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    if (host_wall_time > 0) {
      initial_runtime->kernel->set_wall_time(
          static_cast<std::uint64_t>(host_wall_time));
    }
    output.line("[clock] mode=virtual-rtc seed=host-once rate=realtime "
                "timezone=guest");
  }
  while ((!bounded_execution || remaining_ticks != 0) &&
         !initial_runtime->kernel->process().exited && !hard_stop) {
    std::optional<XnuThreadId> input_preferred_thread;
    refresh_catalog_after_file_mutations(scheduler.runnable_count() == 0);
    if (sdl_display && !sdl_display->poll_events()) {
      hard_stop = true;
      break;
    }
    if (sdl_display) {
      for (const auto &input : sdl_display->take_touch_events()) {
        initial_runtime->kernel->enqueue_touch_input(input);
      }
      for (const auto &input : sdl_display->take_button_events()) {
        initial_runtime->kernel->enqueue_system_button(input);
      }
      for (const auto &input : sdl_display->take_ringer_switch_events()) {
        static_cast<void>(input);
        initial_runtime->kernel->toggle_ringer_switch();
      }
    }
    if (touch_replay) {
      for (const auto &input : touch_replay->poll()) {
        initial_runtime->kernel->enqueue_touch_input(input);
      }
    }
    for (const auto &input : live_touch_scheduler.poll()) {
      initial_runtime->kernel->enqueue_touch_input(input);
    }
    for (const auto &input : live_button_scheduler.poll()) {
      initial_runtime->kernel->enqueue_system_button(input);
      output.line("[control] button=up scheduled event queued");
    }
    if (live_control) {
      for (const auto &command : live_control->poll()) {
        switch (command.kind) {
        case LiveControlCommandKind::Touch:
          initial_runtime->kernel->enqueue_touch_input(command.touch);
          output.line("[control] touch queued");
          break;
        case LiveControlCommandKind::Gesture:
          if (command.wake_display) {
            initial_runtime->kernel->enqueue_system_button(
                SystemButtonInput{SystemButton::Home, SystemButtonPhase::Down});
            initial_runtime->kernel->enqueue_system_button(
                SystemButtonInput{SystemButton::Home, SystemButtonPhase::Up});
            output.line("[control] home requested before unlock gesture");
          }
          live_touch_scheduler.schedule(command.gesture);
          // Preserve stdin command order. In particular, "tap" followed by
          // "lock" begins the touch before the button barrier even though the
          // remainder of the gesture is paced over later host iterations.
          for (const auto &input : live_touch_scheduler.poll()) {
            initial_runtime->kernel->enqueue_touch_input(input);
          }
          output.line(
              "[control] gesture=" + command.message +
              " scheduled events=" + std::to_string(command.gesture.size()));
          break;
        case LiveControlCommandKind::Button:
          initial_runtime->kernel->enqueue_system_button(command.system_button);
          output.line("[control] button event queued");
          break;
        case LiveControlCommandKind::ButtonHold:
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{command.system_button.button,
                                SystemButtonPhase::Down});
          live_button_scheduler.schedule(command.system_button,
                                         command.button_hold);
          output.line("[control] button hold scheduled duration-ms=" +
                      std::to_string(command.button_hold.count()));
          break;
        case LiveControlCommandKind::Home:
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{SystemButton::Home, SystemButtonPhase::Down});
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{SystemButton::Home, SystemButtonPhase::Up});
          output.line("[control] home requested");
          break;
        case LiveControlCommandKind::Lock:
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{SystemButton::Lock, SystemButtonPhase::Down});
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{SystemButton::Lock, SystemButtonPhase::Up});
          output.line("[control] display lock requested");
          break;
        case LiveControlCommandKind::VolumeUp:
        case LiveControlCommandKind::VolumeDown: {
          const auto button = command.kind == LiveControlCommandKind::VolumeUp
                                  ? SystemButton::VolumeUp
                                  : SystemButton::VolumeDown;
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{button, SystemButtonPhase::Down});
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{button, SystemButtonPhase::Up});
          output.line(command.kind == LiveControlCommandKind::VolumeUp
                          ? "[control] volume up requested"
                          : "[control] volume down requested");
          break;
        }
        case LiveControlCommandKind::RingerRing:
        case LiveControlCommandKind::RingerSilent: {
          const auto active =
              command.kind == LiveControlCommandKind::RingerRing;
          initial_runtime->kernel->set_ringer_switch_active(active);
          output.line(active ? "[control] ringer set to ring"
                             : "[control] ringer set to silent");
          break;
        }
        case LiveControlCommandKind::Snapshot: {
          FrameFilePresenter snapshot_writer{command.path};
          const auto frame = initial_runtime->kernel->display_snapshot();
          snapshot_writer.present(frame);
          output.line("[control] snapshot=" + command.path.string() +
                      " frame=" + std::to_string(frame.sequence));
          break;
        }
        case LiveControlCommandKind::SnapshotSequence: {
          const auto start = std::chrono::steady_clock::now();
          for (std::size_t index = 0; index < command.snapshot_count; ++index) {
            std::ostringstream suffix;
            suffix << '-' << std::setfill('0') << std::setw(4) << index
                   << ".ppm";
            scheduled_snapshots.emplace_back(
                start + command.snapshot_interval * index,
                command.path.string() + suffix.str());
          }
          std::stable_sort(scheduled_snapshots.begin(),
                           scheduled_snapshots.end(),
                           [](const auto &left, const auto &right) {
                             return left.first < right.first;
                           });
          output.line(
              "[control] snapshot-sequence prefix=" + command.path.string() +
              " interval-ms=" +
              std::to_string(command.snapshot_interval.count()) +
              " count=" + std::to_string(command.snapshot_count));
          break;
        }
        case LiveControlCommandKind::PerfBegin:
          if (!performance_counters().enabled()) {
            output.line(
                "[control] error: perf-begin requires --perf-summary");
          } else if (display_performance_window) {
            output.line("[control] error: perf window already active label=" +
                        *display_performance_window);
          } else {
            if (sdl_display)
              sdl_display->flush_presentation();
            if (!performance_counters().begin_display_window()) {
              output.line("[control] error: perf window could not begin");
            } else {
              if (performance_counters().cpu_source_diagnostics_configured()) {
                scheduler.set_dispatch_diagnostics(true);
              }
              display_performance_window = command.message;
              // A formal no-content performance window must not fall back to
              // the headless presenter's per-frame PNG writes. The first
              // frame was already captured before the animation window, and
              // explicit snapshots use their own presenter below.
              if (frame_file_presenter && command.message == "animation")
                frame_file_presenter->set_enabled(false);
              if (realtime_pacer) {
                display_clock_window = DisplayClockWindow{
                    std::chrono::steady_clock::now(),
                    initial_runtime->kernel->current_absolute_time(),
                    realtime_pacer->allowed_virtual_time()};
              }
              output.line("[control] perf-begin label=" + command.message);
            }
          }
          break;
        case LiveControlCommandKind::PerfEnd:
          if (!display_performance_window) {
            output.line("[control] error: no active perf window");
          } else {
            const auto clock_ended_at = std::chrono::steady_clock::now();
            const auto guest_ended_at =
                initial_runtime->kernel->current_absolute_time();
            const auto pacer_ended_at =
                realtime_pacer ? realtime_pacer->allowed_virtual_time() : 0U;
            if (performance_counters().cpu_source_diagnostics_configured()) {
              scheduler.set_dispatch_diagnostics(false);
            }
            if (sdl_display)
              sdl_display->flush_presentation();
            const auto snapshot =
                performance_counters().end_display_window();
            if (snapshot) {
              output.line(format_display_performance_summary(
                  *snapshot, *display_performance_window));
            } else {
              output.line("[control] error: perf window could not end");
            }
            if (display_clock_window) {
              const auto host_elapsed = static_cast<std::uint64_t>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      clock_ended_at - display_clock_window->started_at)
                      .count());
              const auto guest_elapsed =
                  guest_ended_at >= display_clock_window->guest_started_at
                      ? guest_ended_at - display_clock_window->guest_started_at
                      : 0U;
              const auto pacer_elapsed =
                  pacer_ended_at >= display_clock_window->pacer_started_at
                      ? pacer_ended_at - display_clock_window->pacer_started_at
                      : 0U;
              output.line(
                  "[perf-clock] label=" + *display_performance_window +
                  " host-ns=" + std::to_string(host_elapsed) +
                  " guest-ns=" + std::to_string(guest_elapsed) +
                  " pacer-ns=" + std::to_string(pacer_elapsed) +
                  " rebase=" +
                  std::to_string(display_clock_window->rebase_count) +
                  " rebase-deficit-total-ns=" +
                  std::to_string(
                      display_clock_window->rebase_deficit_total_nanoseconds) +
                  " rebase-deficit-max-ns=" +
                  std::to_string(
                      display_clock_window->rebase_deficit_max_nanoseconds));
            }
            if (frame_file_presenter)
              frame_file_presenter->set_enabled(true);
            display_clock_window.reset();
            display_performance_window.reset();
          }
          break;
        case LiveControlCommandKind::Status: {
          const auto submitted_frame =
              initial_runtime->kernel->display_submitted_frames();
          const auto frame = sdl_display ? sdl_display->presented_frames()
                                         : submitted_frame;
          const auto active_process =
              initial_runtime->kernel->active_client_process_id();
          output.line(
              "[control] status frame=" + std::to_string(frame) +
              " submitted-frame=" + std::to_string(submitted_frame) +
              " processes=" + std::to_string(runtimes.size()) +
              " threads=" + std::to_string(scheduler.thread_count()) +
              " runnable=" + std::to_string(scheduler.runnable_count()) +
              " active-process=" +
              (active_process ? std::to_string(*active_process) : "none") +
              " display-power=" +
              (initial_runtime->kernel->display_powered_on() ? "on" : "off"));
          break;
        }
        case LiveControlCommandKind::Help:
          output.line("[control] commands: touch down|move|up|cancel x y; "
                      "tap x y [hold-ms]; unlock; "
                      "drag x1 y1 x2 y2 [duration-ms] [steps]; "
                      "button home|lock|volume-up|volume-down down|up; "
                      "hold BUTTON DURATION-MS; home; lock [hold-ms]; "
                      "volume-up; volume-down; snapshot PATH; "
                      "ringer ring|silent; "
                      "snapshot-sequence PATH-PREFIX INTERVAL-MS COUNT; "
                      "perf-begin LABEL; perf-end; "
                      "status; quit");
          break;
        case LiveControlCommandKind::Quit:
          output.line("[control] quit requested");
          hard_stop = true;
          break;
        case LiveControlCommandKind::Error:
          output.line("[control] error: " + command.message);
          break;
        }
      }
      if (hard_stop)
        break;
    }
    while (!scheduled_snapshots.empty() &&
           std::chrono::steady_clock::now() >=
               scheduled_snapshots.front().first) {
      FrameFilePresenter snapshot_writer{scheduled_snapshots.front().second};
      snapshot_writer.present(initial_runtime->kernel->display_snapshot());
      output.line("[control] snapshot-sequence frame=" +
                  scheduled_snapshots.front().second.string());
      scheduled_snapshots.erase(scheduled_snapshots.begin());
    }
    const auto resolve_display_urgent_thread = [&]() {
      if (!display_urgent_process || display_urgent_thread)
        return;
      for (auto &runtime : runtimes) {
        if (runtime->kernel->process().pid != *display_urgent_process ||
            runtime->kernel->process().exited) {
          continue;
        }
        if (const auto processor =
                runtime->kernel->display_vsync_receiver_processor()) {
          display_urgent_thread = XnuThreadId{
              *display_urgent_process,
              static_cast<std::uint32_t>(*processor)};
        }
        break;
      }
    };
    resolve_display_urgent_thread();
    if (realtime_pacer) {
      const auto current_time =
          initial_runtime->kernel->current_absolute_time();
      const auto host_time = realtime_pacer->allowed_virtual_time();
      if (current_time < host_time) {
        if (scheduler.runnable_count() == 0) {
          // Guest execution advances virtual time in calibrated instruction
          // quanta. Catch it up from the host monotonic clock only while all
          // guest threads are idle; forcing wall time through a CPU-bound
          // guest skips animation timers before it can produce their frames.
          initial_runtime->kernel->advance_absolute_time(host_time);
          for (auto &runtime : runtimes) {
            if (runtime.get() != initial_runtime &&
                !runtime->kernel->process().exited) {
              runtime->kernel->service_time_dependent_devices(host_time);
            }
          }
        } else {
          // Short host-side execution and HLE work belongs inside the current
          // display period. Preserve that phase so the following sleep is
          // shortened instead of making every frame take work + one period.
          // Rebase only sustained overload; this still bounds the timer jump
          // when a CPU-bound guest eventually becomes idle.
          const auto deficit = host_time - current_time;
          if (deficit >
              iokit_abi::display_vsync::period_absolute_time) {
            if (display_clock_window) {
              ++display_clock_window->rebase_count;
              display_clock_window->rebase_deficit_total_nanoseconds +=
                  deficit;
              display_clock_window->rebase_deficit_max_nanoseconds =
                  std::max(
                      display_clock_window->rebase_deficit_max_nanoseconds,
                      deficit);
            }
            realtime_pacer.emplace(current_time);
          }
        }
      }
      const auto display_urgent_runnable = [&]() {
        if (!display_urgent_thread)
          return false;
        if (const auto info = scheduler.info(*display_urgent_thread);
            info.has_value()) {
          return info->state == XnuThreadState::Runnable;
        }
        return false;
      }();
      const auto guest_ahead_delay = display_urgent_runnable
          ? std::chrono::nanoseconds::zero()
          : realtime_pacer->delay_until(
                initial_runtime->kernel->current_absolute_time());
      if (guest_ahead_delay > std::chrono::nanoseconds::zero()) {
        const auto sleep_delay = realtime_pacer->limit_delay(
            guest_ahead_delay, next_host_control_deadline());
        if (sleep_delay > std::chrono::nanoseconds::zero()) {
          wait_for_host_activity(sleep_delay);
        }
        // A due host control should wake the polling loop, not make a guest
        // that is still ahead appear eligible to execute.
        continue;
      }
    }
    for (auto &runtime : runtimes) {
      for (std::size_t processor = 0; processor < runtime->cpus->size();
           ++processor) {
        const XnuThreadId thread{runtime->kernel->process().pid,
                                 static_cast<std::uint32_t>(processor)};
        const auto scheduling_info = scheduler.info(thread);
        if (!runtime->allocated[processor] || !scheduling_info ||
            (scheduling_info->state != XnuThreadState::Waiting &&
             scheduling_info->state != XnuThreadState::Runnable)) {
          continue;
        }
        auto &waiting_cpu = runtime->cpus->cpu(processor);
        if (runtime->kernel->deliver_pending_event(waiting_cpu)) {
          const auto delivered_input =
              runtime->kernel->take_last_delivered_graphics_input(processor);
          if (scheduler.make_runnable(thread) && delivered_input) {
            // A just-delivered input event is a generic interactive wakeup,
            // not a process-specific priority. Let its receiver run once
            // before ordinary runnable continuations consume another slice.
            // The preference is scoped to this host-loop iteration and does
            // not alter Guest priority, quantum, wait/wake state, or clocks.
            input_preferred_thread = thread;
            performance_counters().record_diagnostic_input_runnable(
                *delivered_input, thread.process, thread.thread);
          }
        }
      }
    }
    // XNU keeps a compact zombie process record until its parent waits, but
    // the dead task's translated host code has no guest-visible lifetime.
    // Give the compositor a short, generic grace interval before background
    // reclamation: destroying a large JIT immediately after Home otherwise
    // competes with the exit animation for host CPU. The FIFO reaper destroys
    // the pool before any later retirement of the owning Runtime, preserving
    // the AddressSpace and exclusive-monitor lifetimes.
    constexpr auto execution_reclaim_grace = std::chrono::milliseconds{1500};
    const auto reclaim_now = std::chrono::steady_clock::now();
    const auto precompile_finished = [&](Runtime &runtime) {
      if (!runtime.precompile_task) return true;
      if (!runtime.precompile_task->finished()) {
        static_cast<void>(runtime.begin_image_transition(host_resources));
        return false;
      }
      runtime.precompile_task.reset();
      return true;
    };
    for (auto &runtime : runtimes) {
      if (runtime->kernel->process().exited &&
          runtime->cpus->has_execution_resources()) {
        if (!runtime->execution_reclaim_after) {
          runtime->execution_reclaim_after =
              reclaim_now + execution_reclaim_grace;
        }
        if (reclaim_now < *runtime->execution_reclaim_after ||
            scheduler.process_runnable_count(
                runtime->kernel->process().pid) != 0) {
          continue;
        }
        if (!precompile_finished(*runtime)) continue;
        runtime_reaper.retire_execution_resources(
            runtime->cpus->release_execution_resources());
        runtime->execution_reclaim_after.reset();
      }
    }
    for (auto &parent : runtimes) {
      const auto pending_waits = parent->kernel->pending_waits();
      for (const auto &[processor, pending] : pending_waits) {
        const auto child = parent->kernel->wait_child(
            pending.target_pid, false);
        if (child.child_pid &&
            parent->kernel->complete_wait(
                parent->cpus->cpu(processor), *child.child_pid,
                child.status)) {
          static_cast<void>(parent->kernel->wait_child(
              static_cast<std::int32_t>(*child.child_pid), true));
          static_cast<void>(scheduler.make_runnable(
              XnuThreadId{parent->kernel->process().pid,
                          static_cast<std::uint32_t>(processor)}));
        } else if (!child.has_child) {
          if (parent->kernel->fail_wait(parent->cpus->cpu(processor), 10)) {
            static_cast<void>(scheduler.make_runnable(
                XnuThreadId{parent->kernel->process().pid,
                            static_cast<std::uint32_t>(processor)}));
          }
        }
      }
    }
    for (auto runtime = runtimes.begin(); runtime != runtimes.end();) {
      if (runtime->get() != initial_runtime &&
          (*runtime)->kernel->process().exited &&
          !(*runtime)->cpus->has_execution_resources()) {
        if (runtime->get() == display_scanout_owner)
          display_scanout_owner = nullptr;
        if (!precompile_finished(**runtime)) {
          ++runtime;
          continue;
        }
        runtime_index.erase(**runtime);
        runtime_reaper.retire(std::move(*runtime));
        runtime = runtimes.erase(runtime);
      } else {
        ++runtime;
      }
    }
    std::optional<XnuThreadId> preferred_thread;
    if (debug_request && debug_request->thread &&
        debug_request->thread->thread != 0) {
      preferred_thread = XnuThreadId{debug_request->thread->process,
                                     debug_request->thread->thread - 1U};
    }
    if (!preferred_thread && display_urgent_thread) {
      if (const auto info = scheduler.info(*display_urgent_thread);
          info && info->state == XnuThreadState::Runnable) {
        preferred_thread = display_urgent_thread;
      }
    }
    if (!preferred_thread && input_preferred_thread) {
      if (const auto info = scheduler.info(*input_preferred_thread);
          info && info->state == XnuThreadState::Runnable) {
        preferred_thread = input_preferred_thread;
      }
    }
    display_urgent_thread.reset();
    display_urgent_process.reset();
    std::vector<XnuScheduledSlice> scheduled_batch;
    scheduled_batch.reserve(guest_processor_count);
    auto reservable_ticks = remaining_ticks;
    for (std::size_t processor = 0; processor < guest_processor_count;
         ++processor) {
      if (bounded_execution && reservable_ticks == 0)
        break;
      const auto scheduled =
          scheduler.choose_next(processor, preferred_thread);
      if (scheduled) {
        performance_counters().record_diagnostic_input_execute(
            scheduled->thread.process, scheduled->thread.thread);
        if (performance_counters().cpu_source_diagnostics_enabled() &&
            scheduled->runnable_since !=
                std::chrono::steady_clock::time_point{}) {
          const auto elapsed =
              std::chrono::steady_clock::now() - scheduled->runnable_since;
          const auto nanoseconds = static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                  .count());
          performance_counters().record_diagnostic_scheduler_dispatch(
              scheduled->thread.process, scheduled->thread.thread,
              scheduled->runnable_generation, scheduled->front_continuation,
              nanoseconds);
        }
        scheduled_batch.push_back(*scheduled);
        if (bounded_execution) {
          reservable_ticks -=
              std::min(reservable_ticks, scheduled->tick_budget);
        }
        if (scheduled_batch.size() == 1U &&
            guest_processor_count > 1U &&
            guest_parallelism_policy.should_serialize(
                scheduled->thread)) {
          break;
        }
      }
      // A debugger-selected thread is the only thread allowed to make
      // progress for this resume request.
      if (preferred_thread)
        break;
    }

    std::vector<PreparedGuestSlice> prepared_slices;
    prepared_slices.reserve(scheduled_batch.size());
    std::optional<std::uint64_t> display_vsync_tick_budget;
    if (const auto deadline =
            initial_runtime->kernel->next_display_vsync_deadline()) {
      const auto now = initial_runtime->kernel->current_absolute_time();
      if (*deadline > now) {
        display_vsync_tick_budget = std::max<std::uint64_t>(
            1, duration_to_guest_ticks(
                   *deadline - now,
                   darwin::mach::thread_policy::absolute_time_units_per_second,
                   guest_ticks_per_second));
      }
    }
    auto batch_ticks = remaining_ticks;
    const auto host_slice_now = std::chrono::steady_clock::now();
    const auto host_control_deadline = next_host_control_deadline();
    const auto host_slice_budget = [&]() {
      constexpr auto minimum = std::chrono::duration_cast<
          std::chrono::nanoseconds>(std::chrono::microseconds{250});
      constexpr auto nominal = std::chrono::duration_cast<
          std::chrono::nanoseconds>(std::chrono::milliseconds{2});
      constexpr auto maximum = std::chrono::duration_cast<
          std::chrono::nanoseconds>(std::chrono::milliseconds{4});
      constexpr auto translation_safety = std::chrono::duration_cast<
          std::chrono::nanoseconds>(std::chrono::microseconds{100});
      auto budget = nominal;
      const auto measured_block = std::chrono::nanoseconds{
          static_cast<std::chrono::nanoseconds::rep>(
              performance_counters().jit_block_compile_p99_nanoseconds())};
      if (measured_block > std::chrono::nanoseconds::zero()) {
        budget = std::min(
            maximum,
            std::max(budget, measured_block + translation_safety));
      }
      const auto limit_budget = [&](std::chrono::nanoseconds until) {
        if (until <= std::chrono::nanoseconds::zero()) {
          budget = minimum;
          return;
        }
        budget = std::min(
            budget,
            std::max(minimum, until / 2));
      };
      if (host_control_deadline) {
        limit_budget(std::chrono::duration_cast<std::chrono::nanoseconds>(
            *host_control_deadline - host_slice_now));
      }
      if (realtime_pacer) {
        if (const auto display_deadline =
                initial_runtime->kernel->next_display_vsync_deadline()) {
          limit_budget(realtime_pacer->delay_until(*display_deadline));
        }
      }
      return std::max(minimum, std::min(maximum, budget));
    }();
    for (const auto &scheduled_value : scheduled_batch) {
      if (!scheduler.contains(scheduled_value.thread))
        continue;
      Runtime *selected_runtime = nullptr;
      for (auto &candidate : runtimes) {
        if (candidate->kernel->process().pid ==
            scheduled_value.thread.process) {
          selected_runtime = candidate.get();
          break;
        }
      }
      if (selected_runtime == nullptr ||
          scheduled_value.thread.thread >= selected_runtime->cpus->size()) {
        throw std::runtime_error{"scheduler selected an unknown guest thread"};
      }
      if (selected_runtime->kernel->process().exited) {
        scheduler.remove_process(selected_runtime->kernel->process().pid);
        guest_parallelism_policy.forget_process(
            selected_runtime->kernel->process().pid);
        continue;
      }
      const auto index =
          static_cast<std::size_t>(scheduled_value.thread.thread);
      auto &cpu = selected_runtime->cpus->cpu(index);
      cpu.clear_halt();
      auto slice =
          bounded_execution ? std::min(batch_ticks, scheduled_value.tick_budget)
                            : scheduled_value.tick_budget;
      if (display_vsync_tick_budget &&
          *display_vsync_tick_budget < slice) {
        performance_counters().record_display_vsync_budget(
            slice, *display_vsync_tick_budget);
        slice = *display_vsync_tick_budget;
      }
      if (bounded_execution)
        batch_ticks -= slice;
      prepared_slices.push_back(PreparedGuestSlice{
          scheduled_value,
          selected_runtime,
          index,
          &cpu,
          slice,
          host_slice_budget,
          debug_request && debug_request->kind == GdbResumeKind::Step,
          false,
      });
    }

    const auto parallel_guest_batch =
        prepared_slices.size() > 1U &&
        std::none_of(
            prepared_slices.begin(), prepared_slices.end(),
            [&guest_parallelism_policy](const auto &prepared) {
              return guest_parallelism_policy.should_serialize(
                  prepared.scheduled.thread);
            });
    for (auto &prepared : prepared_slices) {
      prepared.deferred_svc = parallel_guest_batch;
      prepared.cpu->set_svc_dispatch_mode(
          parallel_guest_batch ? SvcDispatchMode::Deferred
                               : SvcDispatchMode::Immediate);
    }

    if (guest_processor_count == 1 && !prepared_slices.empty() &&
        last_serial_thread !=
            std::optional<XnuThreadId>{
                prepared_slices.front().scheduled.thread}) {
      // A local ARM exclusive reservation belongs to the physical processor,
      // not to the saved register context. Clear it only at a real serialized
      // thread switch; repeated slices of the same thread retain the ordinary
      // Dynarmic fast path.
      prepared_slices.front().cpu->clear_exclusive_state(
          prepared_slices.front().scheduled.processor);
      last_serial_thread = prepared_slices.front().scheduled.thread;
    }
    if (parallel_guest_batch) {
      guest_slice_workers->run(prepared_slices);
    } else {
      for (auto &prepared : prepared_slices) {
        if (scheduler.contains(prepared.scheduled.thread))
          GuestSliceWorkerPool::execute(prepared);
      }
    }

    const bool ran_thread = !prepared_slices.empty();
    std::uint64_t scheduler_round_ticks = 0;
    for (auto &prepared : prepared_slices) {
      if (prepared.error)
        std::rethrow_exception(prepared.error);
      const auto scheduled =
          std::optional<XnuScheduledSlice>{prepared.scheduled};
      if (!scheduler.contains(scheduled->thread))
        continue;
      auto &runtime = *prepared.runtime;
      const auto index = prepared.thread_index;
      auto &cpu = *prepared.cpu;
      auto result = std::move(prepared.result);
      guest_parallelism_policy.observe(
          scheduled->thread, result.ticks_consumed, result.svc_calls);
      if (prepared.deferred_svc && result.svc) {
        runtime.kernel->dispatch(cpu, *result.svc);
        // UserDefined2 is shared by deferred SVC and host cooperation. The
        // explicit host-only marker remains attached to the result; only the
        // reason explicitly requested by the serial kernel dispatch represents
        // the guest thread's scheduler state here.
        result.reason = cpu.consume_requested_halt_reason();
      }
      scheduler_round_ticks =
          std::max(scheduler_round_ticks, result.ticks_consumed);
      stopped_pid = runtime.kernel->process().pid;
      stopped_cpu = index;
      stopped_result = result;
      consumed_ticks += result.ticks_consumed;
      if (bounded_execution) {
        remaining_ticks -= std::min(remaining_ticks, result.ticks_consumed);
      }
      bool debug_stop =
          result.debug_breakpoint.has_value() || prepared.single_step;
      std::uint8_t debug_signal = gdb_signal::trap;
      const auto fatal_result =
          result.fault || !result.exception.empty() ||
          Dynarmic::Has(result.reason, Dynarmic::HaltReason::UserDefined4);
      if (fatal_result) {
        const auto &registers = cpu.registers();
        std::ostringstream failure;
        failure << "[cpu] fatal pid=" << runtime.kernel->process().pid
                << " cpu=" << index << " pc=0x" << std::hex << registers[15]
                << " lr=0x" << registers[14];
        if (result.fault) {
          failure << " fault=0x" << result.fault->address << " access=0x"
                  << static_cast<unsigned>(result.fault->access)
                  << " size=0x" << result.fault->size;
        }
        if (!result.exception.empty())
          failure << " exception=\"" << result.exception << '"';
        output.line(failure.str());
      }
      auto completion = XnuSliceCompletion::Continue;
      bool scheduler_completed = false;
      if (Dynarmic::Has(result.reason, Dynarmic::HaltReason::UserDefined5)) {
        completion = XnuSliceCompletion::Block;
      } else if (Dynarmic::Has(result.reason,
                               Dynarmic::HaltReason::UserDefined6) &&
                 runtime.pending_exec) {
        auto pending = std::move(*runtime.pending_exec);
        runtime.pending_exec.reset();
        const auto image_epoch = runtime.begin_image_transition(host_resources);
        try {
          refresh_catalog_after_file_mutations(true);
          debug_target.notify_exec(runtime.kernel->process().pid);
          runtime.memory->clear();
          ProcessLoader exec_loader{*rootfs, *runtime.memory,
                                    guest_architecture, catalog_index};
          auto loaded =
              exec_loader.load(pending.path, std::move(pending.arguments),
                               pending.environment);
          runtime.kernel->set_process_arguments(loaded.arguments,
                                                pending.environment);
          runtime.kernel->set_process_image(
              pending.path, loaded.executable.code_signature_entitlements());
          const auto catalog_generation_pending = assign_jit_process_profile(
              runtime, loaded,
              precompile_phase_for_process(loaded.executable_path));
          runtime.kernel->prepare_exec(pending.processor);
          auto &exec_cpu = runtime.cpus->cpu(pending.processor);
          exec_cpu.reset();
          exec_cpu.clear_cache();
          exec_cpu.registers().fill(0);
          exec_cpu.registers()[13] = loaded.stack_pointer;
          exec_cpu.registers()[15] = loaded.entry_point;
          exec_cpu.set_cpsr(0x10);
          runtime.kernel->install_main_image_hle(exec_cpu,
                                                 loaded.executable_path);
          precompile_catalog_generation(runtime, catalog_generation_pending,
                                        loaded.executable_path);
          runtime.activate_image_epoch(image_epoch);
          static_cast<void>(scheduler.complete_slice(
              scheduled->thread, result.ticks_consumed,
              XnuSliceCompletion::Terminate, XnuTimeAccounting::Deferred));
          scheduler.remove_process(runtime.kernel->process().pid);
          guest_parallelism_policy.forget_process(
              runtime.kernel->process().pid);
          std::fill(runtime.allocated.begin(), runtime.allocated.end(), false);
          runtime.allocated[pending.processor] = true;
          static_cast<void>(scheduler.register_thread(
              XnuThreadId{runtime.kernel->process().pid,
                          static_cast<std::uint32_t>(pending.processor)},
              runtime.kernel->process().thread_base_priority));
          scheduler_completed = true;
        } catch (const std::exception &error) {
          output.line("[process] exec failed pid=" +
                      std::to_string(runtime.kernel->process().pid) +
                      " path=" + pending.path + " error=" + error.what());
          runtime.kernel->exit_process(127);
          completion = XnuSliceCompletion::Terminate;
        }
      } else if (fatal_result) {
        if (gdb_server) {
          debug_stop = true;
          debug_signal = result.fault ? gdb_signal::segmentation_fault
                                      : gdb_signal::illegal_instruction;
        } else if (runtime.kernel->process().pid !=
                   initial_runtime->kernel->process().pid) {
          runtime.kernel->exit_process(
              0, result.fault ? gdb_signal::segmentation_fault
                              : gdb_signal::illegal_instruction);
          completion = XnuSliceCompletion::Terminate;
        } else {
          completion = XnuSliceCompletion::Terminate;
          hard_stop = true;
        }
      } else if (Dynarmic::Has(result.reason,
                               Dynarmic::HaltReason::CacheInvalidation)) {
        // Dynarmic may return after completing a shared code-cache
        // invalidation at a safe host boundary. This is not a guest wait or
        // scheduler state transition; resume the same runnable slice.
        completion = XnuSliceCompletion::Continue;
      } else if (Dynarmic::Has(result.reason,
                               Dynarmic::HaltReason::UserDefined1)) {
        completion = XnuSliceCompletion::Terminate;
      } else if (Dynarmic::Has(result.reason,
                               Dynarmic::HaltReason::UserDefined8)) {
        if (const auto request = runtime.kernel->consume_scheduler_yield(index);
            request && request->depress) {
          const auto duration_ticks =
              duration_to_guest_ticks(
                  request->duration_milliseconds,
                  xnu792::scheduler::milliseconds_per_second,
                  guest_ticks_per_second);
          static_cast<void>(
              scheduler.depress(scheduled->thread, duration_ticks));
        }
        completion = XnuSliceCompletion::Yield;
      } else if (result.host_yielded) {
        // Host cooperation is not a Guest AST or a guest scheduler yield.
        // Keep the runnable thread at the head of its current quantum.
        completion = XnuSliceCompletion::Continue;
      } else if (Dynarmic::Has(result.reason,
                               Dynarmic::HaltReason::UserDefined2)) {
        // XNU AST preemption retains the current quantum. The
        // scheduler requeues this thread at the head of its
        // priority, while a higher priority still wins selection.
        completion = XnuSliceCompletion::Continue;
      } else if (result.ticks_consumed == 0 && !debug_stop) {
        // A runnable CPU returning without executing an instruction
        // and without a classified wait/exit/fault would otherwise
        // make the unbounded scheduler spin forever. This is an
        // internal emulation failure, not a normal stop condition.
        std::ostringstream error;
        error << "scheduler made no progress for pid="
              << runtime.kernel->process().pid << " cpu=" << index << " pc=0x"
              << std::hex << cpu.registers()[15] << " halt_reason=0x"
              << static_cast<std::uint64_t>(result.reason);
        throw std::runtime_error{error.str()};
      }
      if (!scheduler_completed) {
        static_cast<void>(
            scheduler.complete_slice(scheduled->thread, result.ticks_consumed,
                                     completion, XnuTimeAccounting::Deferred));
        if (completion == XnuSliceCompletion::Terminate &&
            runtime.kernel->process().exited) {
          scheduler.remove_process(runtime.kernel->process().pid);
          guest_parallelism_policy.forget_process(
              runtime.kernel->process().pid);
        }
      }
      if (gdb_server && gdb_server->poll_interrupt()) {
        debug_stop = true;
        debug_signal = gdb_signal::interrupt;
      }
      if (debug_stop && gdb_server && !hard_stop) {
        const GdbThreadId stopped_thread{
            runtime.kernel->process().pid,
            static_cast<std::uint32_t>(index + 1U)};
        debug_target.set_current_thread(stopped_thread);
        auto request = gdb_server->command_loop(debug_target, stopped_thread,
                                                debug_signal, true);
        if (request.kind == GdbResumeKind::Detach) {
          debug_target.remove_all_breakpoints();
          gdb_server->detach();
          gdb_server.reset();
          debug_request.reset();
          for (auto &candidate : runtimes) {
            for (std::size_t processor = 0; processor < candidate->cpus->size();
                 ++processor) {
              candidate->cpus->cpu(processor).set_debug_breakpoints_enabled(
                  false);
            }
          }
        } else if (request.kind == GdbResumeKind::Kill) {
          hard_stop = true;
        } else {
          debug_request = request;
        }
      }
      if (hard_stop)
        break;
    }
    const auto display_deadline_before_advance =
        initial_runtime->kernel->next_display_vsync_deadline();
    const auto display_time_before_advance =
        initial_runtime->kernel->current_absolute_time();
    scheduler.advance_time(scheduler_round_ticks);
    if (scheduler_round_ticks != 0) {
      initial_runtime->kernel->advance_time_by(
          guest_tick_clock.absolute_time_units(scheduler_round_ticks));
      const auto advanced_time =
          initial_runtime->kernel->current_absolute_time();
      for (auto &runtime : runtimes) {
        if (runtime.get() != initial_runtime &&
            !runtime->kernel->process().exited) {
          runtime->kernel->service_time_dependent_devices(advanced_time);
        }
      }
      // AppleH1CLCD scans its reserved CoreSurface directly; firmware does
      // not unlock or swap that front buffer. Cache the publishing task after
      // the first lookup; imported task-local mappings retain the producer
      // provenance and cannot steal ownership.
      if (display_scanout_owner == nullptr ||
          display_scanout_owner->kernel->process().exited ||
          !display_scanout_owner->kernel->owns_display_scanout()) {
        display_scanout_owner = nullptr;
        for (auto &runtime : runtimes) {
          if (!runtime->kernel->process().exited &&
              runtime->kernel->owns_display_scanout()) {
            display_scanout_owner = runtime.get();
            output.line("[display] scanout-owner pid=" +
                        std::to_string(
                            display_scanout_owner->kernel->process().pid));
            break;
          }
        }
      }
      if (display_scanout_owner != nullptr)
        static_cast<void>(
            display_scanout_owner->kernel->refresh_display_scanout());
      if (display_deadline_before_advance &&
          *display_deadline_before_advance > display_time_before_advance &&
          advanced_time >= *display_deadline_before_advance &&
          display_scanout_owner != nullptr) {
        display_urgent_process =
            display_scanout_owner->kernel->process().pid;
      }
    }
    const auto display_submissions =
        initial_runtime->kernel->display_submitted_frames();
    if (display_submissions != observed_display_submissions) {
      observed_display_submissions = display_submissions;
      last_display_submission = std::chrono::steady_clock::now();
    }
    if (ran_thread) {
      guest_idle_since.reset();
    } else if (!guest_idle_since) {
      guest_idle_since = std::chrono::steady_clock::now();
    }
    if (!ran_thread) {
      constexpr auto jit_quota_refresh_period = std::chrono::milliseconds{250};
      const auto quota_now = std::chrono::steady_clock::now();
      if (quota_now - last_jit_quota_refresh >= jit_quota_refresh_period) {
        last_jit_quota_refresh = quota_now;
        const auto memory = host_memory_budget_snapshot();
        latest_host_memory_budget = memory;
        const auto memory_pressured = host_memory_is_pressured(memory);
        jit_code_cache_governor.set_pressure_limited(memory_pressured);
        if (memory_pressured && !pressure_reclamation_applied) {
          // Pressure handling is ordered: cancel optional compile work first,
          // then stop artifact writeback, then reclaim unreferenced artifacts.
          for (auto &runtime : runtimes) {
            if (runtime->precompile_task &&
                !runtime->precompile_task->finished()) {
              runtime->precompile_task->cancel();
              runtime->cpus->quiesce_precompilation();
              runtime->precompile_task->wait_finished();
            }
          }
          jit_artifacts->cancel_writeback();
          const auto artifact_before = jit_artifacts->stats();
          const auto target = artifact_before.resident_bytes / 2U;
          const auto reclaimed = jit_artifacts->trim_resident_bytes(target);
          output.line("[jit-pressure] level=" +
                      std::string{host_memory_pressure_name(memory)} +
                      " compile=stopped writeback=stopped "
                      "artifact-reclaimed-bytes=" +
                      std::to_string(reclaimed) + " target-resident-bytes=" +
                      std::to_string(target));
          pressure_reclamation_applied = true;
        } else if (!memory_pressured) {
          pressure_reclamation_applied = false;
        }
        for (auto &runtime : runtimes) {
          if (!runtime->jit_cache_reservation ||
              !runtime->cpus->has_execution_resources()) {
            continue;
          }
          const auto actual = runtime->cpus->jit_code_cache_bytes();
          static_cast<void>(jit_code_cache_governor.refresh_actual(
              *runtime->jit_cache_reservation, actual, memory));
        }
      }
      if (gdb_server && gdb_server->poll_interrupt()) {
        const auto stopped_thread =
            debug_target.current_thread().value_or(GdbThreadId{1, 1});
        auto request = gdb_server->command_loop(debug_target, stopped_thread,
                                                gdb_signal::interrupt, true);
        if (request.kind == GdbResumeKind::Detach) {
          debug_target.remove_all_breakpoints();
          gdb_server->detach();
          gdb_server.reset();
          debug_request.reset();
          for (auto &runtime : runtimes) {
            for (std::size_t processor = 0; processor < runtime->cpus->size();
                 ++processor) {
              runtime->cpus->cpu(processor).set_debug_breakpoints_enabled(
                  false);
            }
          }
        } else if (request.kind == GdbResumeKind::Kill) {
          hard_stop = true;
        } else {
          debug_request = request;
        }
        continue;
      }
      std::optional<std::uint64_t> next_deadline;
      for (const auto &runtime : runtimes) {
        const auto process_id = runtime->kernel->process().pid;
        const auto deadline = runtime->kernel->next_timer_deadline();
        if (deadline)
          guest_deadlines.upsert(process_id, *deadline);
        else
          guest_deadlines.erase(process_id);
      }
      next_deadline = guest_deadlines.next_deadline();
      Runtime *active_runtime = nullptr;
      if (const auto active_process =
              initial_runtime->kernel->active_client_process_id()) {
        active_runtime = runtime_index.find(*active_process);
      }
      constexpr std::size_t idle_precompile_block_budget = 32;
      constexpr std::uint64_t idle_precompile_time_budget_ns = 500000;
      // A Dynarmic block compile is not preemptible. Keep a conservative
      // reserve beyond the nominal batch budget so profile warming cannot
      // consume the next guest timer or host-input deadline.
      const auto historical_block_reserve = std::chrono::nanoseconds{
          static_cast<std::chrono::nanoseconds::rep>(std::max(
              performance_counters().jit_block_compile_p95_nanoseconds(),
              performance_counters().jit_block_compile_p99_nanoseconds()))};
      // Keep the existing conservative floor until enough history exists;
      // then let the measured P95/P99 of one non-preemptible block extend it.
      const auto idle_precompile_deadline_reserve = std::max(
          std::chrono::nanoseconds{std::chrono::milliseconds{20}},
          historical_block_reserve);
      constexpr auto idle_precompile_display_quiet_period =
          std::chrono::milliseconds{100};
      constexpr auto idle_precompile_no_deadline_quiet_period =
          std::chrono::milliseconds{20};
      const auto host_memory_pressure = [&]() {
        return host_memory_is_pressured(latest_host_memory_budget);
      };
      const auto available_precompile_budget =
          [&](HostWorkKind work_kind) {
        const auto memory_pressure = host_memory_pressure();
        // Both optional compile streams yield under pressure. Demand JIT for
        // a running foreground process remains available through the normal
        // execution path and is never submitted as background work here.
        if (memory_pressure &&
            (work_kind == HostWorkKind::OfflineCompile ||
             work_kind == HostWorkKind::BackgroundCompile))
          return PrecompileBudgetDecision{
              0U, PrecompileScheduleSkip::MemoryPressure};
        if (!realtime_pacer)
          return PrecompileBudgetDecision{
              memory_pressure ? idle_precompile_time_budget_ns / 4U
                               : idle_precompile_time_budget_ns,
              PrecompileScheduleSkip::ZeroBudget};
        const auto now = std::chrono::steady_clock::now();
        if (now - last_display_submission <
            idle_precompile_display_quiet_period) {
          return PrecompileBudgetDecision{
              0U, PrecompileScheduleSkip::DisplayQuiet};
        }
        auto available = std::chrono::nanoseconds::max();
        if (next_deadline) {
          available = realtime_pacer->delay_until(*next_deadline);
        } else if (!guest_idle_since ||
                   now - *guest_idle_since <
                       idle_precompile_no_deadline_quiet_period) {
          return PrecompileBudgetDecision{
              0U, PrecompileScheduleSkip::GuestNotIdle};
        }
        available = realtime_pacer->limit_delay(
            available, next_host_control_deadline());
        if (available <= idle_precompile_deadline_reserve)
          return PrecompileBudgetDecision{
              0U, PrecompileScheduleSkip::DeadlineReserve};
        const auto budget = std::chrono::duration_cast<std::chrono::nanoseconds>(
            available - idle_precompile_deadline_reserve);
        const auto paced_budget = std::min(
            idle_precompile_time_budget_ns,
            static_cast<std::uint64_t>(budget.count()));
        return PrecompileBudgetDecision{
            memory_pressure ? paced_budget / 4U : paced_budget,
            PrecompileScheduleSkip::ZeroBudget};
      };
      const auto next_host_deadline = next_host_control_deadline();
      std::optional<HostResourceController::Clock::time_point>
          host_compile_deadline;
      if (realtime_pacer && next_deadline) {
        const auto delay = realtime_pacer->delay_until(*next_deadline);
        if (delay > std::chrono::nanoseconds::zero()) {
          host_compile_deadline = HostResourceController::Clock::now() + delay;
        }
      }
      if (next_host_deadline &&
          (!host_compile_deadline ||
           *next_host_deadline < *host_compile_deadline)) {
        host_compile_deadline = *next_host_deadline;
      }
      host_resources.set_next_deadline(host_compile_deadline);
      const auto schedule_precompile_runtime =
          [&](Runtime *runtime, HostWorkKind work_kind,
              JitPrecompileTarget target) {
        if (runtime == nullptr || runtime->kernel->process().exited) {
          record_precompile_schedule_skip(PrecompileScheduleSkip::NoRuntime);
          return;
        }
        if (runtime->precompile_task) {
          if (!runtime->precompile_task->finished()) {
            record_precompile_schedule_skip(PrecompileScheduleSkip::TaskBusy);
            return;
          }
          runtime->precompile_task.reset();
        }
        const auto next_phase = runtime->cpus->next_precompile_phase(target);
        if (!next_phase) {
          record_precompile_schedule_skip(PrecompileScheduleSkip::NoPhase);
          return;
        }
        const auto phase = *next_phase;
        const auto budget = available_precompile_budget(work_kind);
        if (budget.budget == 0U) {
          record_precompile_schedule_skip(budget.zero_reason);
          return;
        }
        const auto expected_epoch = runtime->work_epoch.current();
        runtime->precompile_task = host_resources.submit_cancellable(
            work_kind, host_compile_deadline,
            [runtime, expected_epoch, budget, idle_precompile_block_budget,
             phase, target,
             &precompile_blocks_by_phase, &precompile_blocks_by_target,
             &record_precompile_outcomes](const HostWorkToken &token) {
              const auto result = runtime->cpus->precompile_pending(
                  idle_precompile_block_budget, budget.budget, target,
                  [runtime, expected_epoch, &token] {
                    return token.cancelled() ||
                           runtime->precompile_stop_requested(expected_epoch);
                  });
              record_precompile_outcomes(result);
              const auto compiled = target == JitPrecompileTarget::NativeCode
                                        ? result.native_compiled
                                        : result.portable_generated;
              precompile_blocks_by_phase[static_cast<std::size_t>(phase)]
                  .fetch_add(compiled, std::memory_order_relaxed);
              precompile_blocks_by_target[static_cast<std::size_t>(target)]
                  .fetch_add(compiled, std::memory_order_relaxed);
            },
            std::chrono::nanoseconds{
                static_cast<std::chrono::nanoseconds::rep>(budget.budget)});
        if (runtime->precompile_task) {
          ++precompile_tasks_by_phase[static_cast<std::size_t>(phase)];
          ++precompile_tasks_by_target[static_cast<std::size_t>(target)];
        } else {
          record_precompile_schedule_skip(
              PrecompileScheduleSkip::HostRejected);
        }
      };
      schedule_precompile_runtime(active_runtime,
                                  HostWorkKind::BackgroundCompile,
                                  JitPrecompileTarget::NativeCode);
      const auto schedule_artifact_compaction = [&]() {
        if (artifact_compaction_task) {
          if (!artifact_compaction_task->finished()) {
            if (scheduler.runnable_count() != 0) {
              artifact_compaction_task->cancel();
              host_resources.wake();
            }
            return;
          }
          artifact_compaction_task.reset();
        }
        if (scheduler.runnable_count() != 0 ||
            !jit_artifacts->compaction_needed()) {
          return;
        }
        artifact_compaction_task = host_resources.submit(
            HostWorkKind::ArtifactCompaction, host_compile_deadline,
            [jit_artifacts] { static_cast<void>(jit_artifacts->compact()); },
            std::chrono::milliseconds{100});
      };
      schedule_artifact_compaction();
      // The scanout publisher may be a background compositor while an App is
      // active. Its cached exit/unlock paths are just as latency-sensitive as
      // the foreground process, so consume their host-only profile hints while
      // every guest thread is idle.
      if (display_scanout_owner != active_runtime) {
        schedule_precompile_runtime(display_scanout_owner,
                                    HostWorkKind::BackgroundCompile,
                                    JitPrecompileTarget::NativeCode);
      }
      Runtime *offline_precompile_runtime = nullptr;
      std::optional<JitPrecompilePhase> offline_precompile_phase;
      // Active and scanout owners were submitted above for native warming.
      // Use one remaining worker slot to generate reusable portable IR for the
      // earliest phase across every other live process; creation order breaks
      // equal-phase ties and therefore preserves startup-service age.
      for (const auto &runtime : runtimes) {
        if (runtime.get() == active_runtime ||
            runtime.get() == display_scanout_owner ||
            runtime->kernel->process().exited ||
            (runtime->precompile_task &&
             !runtime->precompile_task->finished())) {
          continue;
        }
        const auto phase = runtime->cpus->next_precompile_phase(
            JitPrecompileTarget::PortableIr);
        if (phase &&
            (!offline_precompile_phase ||
             static_cast<std::uint8_t>(*phase) <
                 static_cast<std::uint8_t>(*offline_precompile_phase))) {
          offline_precompile_runtime = runtime.get();
          offline_precompile_phase = phase;
        }
      }
      schedule_precompile_runtime(offline_precompile_runtime,
                                  HostWorkKind::OfflineCompile,
                                  JitPrecompileTarget::PortableIr);
      if (next_deadline) {
        if (realtime_pacer) {
          const auto guest_ahead_delay =
              realtime_pacer->delay_until(*next_deadline);
          if (guest_ahead_delay > std::chrono::nanoseconds::zero()) {
            const auto sleep_delay = realtime_pacer->limit_delay(
                guest_ahead_delay, next_host_control_deadline());
            if (sleep_delay > std::chrono::nanoseconds::zero()) {
              wait_for_host_activity(sleep_delay);
            }
            // Keep guest-time advancement gated by the raw pacing result.
            // A clipped host sleep only makes input polling responsive.
            continue;
          }
        }
        initial_runtime->kernel->advance_absolute_time(*next_deadline);
        for (auto &runtime : runtimes) {
          if (runtime.get() != initial_runtime &&
              !runtime->kernel->process().exited) {
            runtime->kernel->service_time_dependent_devices(*next_deadline);
          }
        }
        continue;
      }
      constexpr auto touch_replay_quiet_period = std::chrono::seconds{2};
      if (bounded_execution && touch_replay &&
          !touch_replay->settled(touch_replay_quiet_period)) {
        // A finite headless run must not terminate during a guest idle window
        // while host-time UI automation still has events scheduled or the
        // guest is draining the final event. Keep the same low-overhead idle
        // behavior as the unbounded interactive loop.
        auto replay_deadline = touch_replay->next_deadline();
        if (!replay_deadline) {
          replay_deadline =
              touch_replay->settled_deadline(touch_replay_quiet_period);
        }
        if (replay_deadline) {
          const auto now = std::chrono::steady_clock::now();
          wait_for_host_activity(
              *replay_deadline > now
                  ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                        *replay_deadline - now)
                  : std::chrono::nanoseconds::zero());
        }
        continue;
      }
      if (bounded_execution)
        break;
      // An interactive emulator remains alive while every guest thread
      // is blocked: wait for the next automation deadline or control input.
      // Standalone SDL sessions block on SDL's event queue; GDB and mixed
      // control sessions retain the bounded compatibility fallback.
      auto delay = std::chrono::nanoseconds::max();
      if (const auto deadline = next_host_control_deadline()) {
        const auto now = std::chrono::steady_clock::now();
        if (*deadline > now) {
          delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
              *deadline - now);
        } else {
          delay = std::chrono::nanoseconds::zero();
        }
      } else if ((!live_control || live_control->closed()) &&
                 (sdl_display || gdb_server)) {
        delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
            sdl_event_poll_fallback);
      }
      if (realtime_pacer)
        delay = realtime_pacer->limit_delay(delay,
                                            next_host_control_deadline());
      wait_for_host_activity(delay);
    }
  }
  const auto checked_in_services =
      initial_runtime->kernel->bootstrap_checked_in_service_count();
  output.line("[boot] milestone=service-check-in service-state=" +
              std::string{checked_in_services == 0 ? "waiting" : "ready"} +
              " checked-in-services=" +
              std::to_string(checked_in_services));
  std::size_t allocated_count = 0;
  std::size_t runnable_count = 0;
  std::size_t waiting_count = 0;
  std::size_t mapped_pages = 0;
  std::size_t resident_pages = 0;
  std::size_t shared_page_mappings = 0;
  std::size_t cached_file_mappings = 0;
  std::size_t mapping_regions = 0;
  Runtime *stopped_runtime = initial_runtime;
  for (auto &runtime : runtimes) {
    mapped_pages += runtime->memory->mapped_page_count();
    resident_pages += runtime->memory->resident_page_count();
    shared_page_mappings += runtime->memory->shared_page_count();
    cached_file_mappings += runtime->memory->cached_file_mapping_count();
    mapping_regions += runtime->memory->mapping_region_count();
    allocated_count +=
        std::count(runtime->allocated.begin(), runtime->allocated.end(), true);
    std::size_t process_runnable = 0;
    std::size_t process_waiting = 0;
    for (std::size_t processor = 0; processor < runtime->allocated.size();
         ++processor) {
      if (!runtime->allocated[processor])
        continue;
      const auto scheduling_info =
          scheduler.info(XnuThreadId{runtime->kernel->process().pid,
                                     static_cast<std::uint32_t>(processor)});
      if (!scheduling_info)
        continue;
      process_runnable += scheduling_info->state == XnuThreadState::Runnable ||
                          scheduling_info->state == XnuThreadState::Running;
      process_waiting += scheduling_info->state == XnuThreadState::Waiting;
    }
    runnable_count += process_runnable;
    waiting_count += process_waiting;
    runtime->kernel->process().waiting_for_events =
        process_runnable == 0 && process_waiting != 0;
    if (!runtime->kernel->process().exited) {
      for (std::size_t processor = 0; processor < runtime->allocated.size();
           ++processor) {
        if (!runtime->allocated[processor])
          continue;
        const auto scheduling_info =
            scheduler.info(XnuThreadId{runtime->kernel->process().pid,
                                       static_cast<std::uint32_t>(processor)});
        const auto runnable =
            scheduling_info &&
            (scheduling_info->state == XnuThreadState::Runnable ||
             scheduling_info->state == XnuThreadState::Running);
        const auto waiting = scheduling_info &&
                             scheduling_info->state == XnuThreadState::Waiting;
        output.line("[scheduler] pid=" +
                    std::to_string(runtime->kernel->process().pid) +
                    " cpu=" + std::to_string(processor) +
                    " runnable=" + std::to_string(runnable) +
                    " waiting=" + std::to_string(waiting) + " priority=" +
                    std::to_string(scheduling_info
                                       ? scheduling_info->scheduled_priority
                                       : -1) +
                    " wait=" + runtime->kernel->wait_reason(processor));
      }
    }
    if (runtime->kernel->process().pid == stopped_pid)
      stopped_runtime = runtime.get();
  }
  std::ostringstream message;
  message << "[cpu] stopped pid=" << stopped_pid << " cpu=" << stopped_cpu
          << " pc=0x" << std::hex
          << stopped_runtime->cpus->cpu(stopped_cpu).registers()[15] << std::dec
          << " ticks=" << consumed_ticks << " processes=" << runtimes.size()
          << " threads=" << allocated_count << " runnable=" << runnable_count
          << " mapped-pages=" << mapped_pages
          << " resident-pages=" << resident_pages
          << " mapping-regions=" << mapping_regions
          << " shared-page-mappings=" << shared_page_mappings
          << " cached-file-mappings=" << cached_file_mappings
          << " cached-file-pages="
          << initial_runtime->memory->cached_file_page_count();
  const auto &stopped_registers =
      stopped_runtime->cpus->cpu(stopped_cpu).registers();
  if (const auto instruction = stopped_runtime->memory->read32(
          stopped_registers[15], MemoryPermission::Execute)) {
    message << " insn=0x" << std::hex << *instruction << "("
            << Dynarmic::A32::DisassembleArm(*instruction) << ")"
            << " lr=0x" << stopped_registers[14] << std::dec;
  }
  if (stopped_result.fault) {
    message << " fault=0x" << std::hex << stopped_result.fault->address
            << " access=" << static_cast<unsigned>(stopped_result.fault->access)
            << " size=0x" << stopped_result.fault->size;
    for (std::size_t index = 0; index < 14; ++index) {
      message << " r" << std::dec << index << "=0x" << std::hex
              << stopped_registers[index];
    }
    message << " stack=";
    for (std::size_t index = 0; index < fault_stack_word_count; ++index) {
      const auto address =
          stopped_registers[13] +
          static_cast<std::uint32_t>(index * sizeof(std::uint32_t));
      const auto word = stopped_runtime->memory->read32(address);
      if (!word)
        break;
      if (index != 0)
        message << ',';
      message << "0x" << *word;
    }
    message << " code=";
    const auto code_base = stopped_registers[15] - 8U * sizeof(std::uint32_t);
    for (std::size_t index = 0; index < 16; ++index) {
      const auto word = stopped_runtime->memory->read32(
          code_base + static_cast<std::uint32_t>(index * 4U));
      if (!word)
        break;
      if (index != 0)
        message << ',';
      message << "0x" << *word;
    }
    message << std::dec;
  }
  if (!stopped_result.exception.empty()) {
    message << " exception=" << stopped_result.exception;
  }
  if (initial_runtime->kernel->process().exited) {
    message << " exit=" << initial_runtime->kernel->process().exit_status;
  }
  if (runnable_count == 0 && waiting_count != 0) {
    message << " state=waiting-for-events";
  }
  output.line(message.str());
  if (baseband_capture_stream) {
    baseband_capture_stream->flush();
    if (!*baseband_capture_stream) {
      throw std::runtime_error{"cannot flush baseband capture output: " +
                               *baseband_output_path};
    }
    output.line("[baseband] capture output=" + *baseband_output_path +
                " bytes=" + std::to_string(baseband_capture_bytes));
  }
  const auto report_performance = flag(args, "--perf-summary");
  if (sdl_display)
    sdl_display->flush_presentation();
  const auto stopped_guest = report_performance
                                 ? performance_counters().snapshot()
                                 : PerformanceSnapshot{};
  const auto file_cache_stats = report_performance
                                    ? initial_runtime->memory->file_page_cache_stats()
                                    : FilePageCacheStats{};
  const auto file_page_cache_bytes =
      static_cast<std::uint64_t>(initial_runtime->memory->cached_file_page_count()) *
      AddressSpace::page_size;
  host_resources.wait_idle();
  refresh_catalog_after_file_mutations(true, false);
  static_cast<void>(host_file_watcher.publish_stable(
      host_resources,
      *initial_runtime->kernel->guest_file_generation_registry(), 0, false));
  const auto host_watch_stats = host_file_watcher.stats();
  output.line(
      "[host-watch] async-scheduled=" +
      std::to_string(host_watch_stats.scheduled) +
      " rejected=" + std::to_string(host_watch_stats.rejected) +
      " completed=" + std::to_string(host_watch_stats.completed) +
      " sha-computations=" +
      std::to_string(host_watch_stats.sha_computations) +
      " sha-bytes=" + std::to_string(host_watch_stats.sha_bytes) +
      " confirmed-changes=" +
      std::to_string(host_watch_stats.confirmed_changes));
  output.line(
      "[precompile] loader=" +
      std::to_string(precompile_tasks_by_phase[0]) + "/" +
      std::to_string(precompile_blocks_by_phase[0].load(
          std::memory_order_relaxed)) +
      " system-ui=" + std::to_string(precompile_tasks_by_phase[1]) + "/" +
      std::to_string(precompile_blocks_by_phase[1].load(
          std::memory_order_relaxed)) +
      " startup-service=" +
      std::to_string(precompile_tasks_by_phase[2]) + "/" +
      std::to_string(precompile_blocks_by_phase[2].load(
          std::memory_order_relaxed)) +
      " foreground-application=" +
      std::to_string(precompile_tasks_by_phase[3]) + "/" +
      std::to_string(precompile_blocks_by_phase[3].load(
          std::memory_order_relaxed)) +
      " remaining=" + std::to_string(precompile_tasks_by_phase[4]) + "/" +
      std::to_string(precompile_blocks_by_phase[4].load(
          std::memory_order_relaxed)) +
      " native=" + std::to_string(precompile_tasks_by_target[0]) + "/" +
      std::to_string(precompile_blocks_by_target[0].load(
          std::memory_order_relaxed)) +
      " portable-ir=" +
      std::to_string(precompile_tasks_by_target[1]) + "/" +
      std::to_string(precompile_blocks_by_target[1].load(
          std::memory_order_relaxed)));
  output.line(
      "[precompile-outcomes] attempted=" +
      std::to_string(precompile_outcomes.attempted.load(
          std::memory_order_relaxed)) +
      " native-compiled=" +
      std::to_string(precompile_outcomes.native_compiled.load(
          std::memory_order_relaxed)) +
      " portable-generated=" +
      std::to_string(precompile_outcomes.portable_generated.load(
          std::memory_order_relaxed)) +
      " portable-artifact-hits=" +
      std::to_string(precompile_outcomes.portable_artifact_hits.load(
          std::memory_order_relaxed)) +
      " artifact-imported=" +
      std::to_string(precompile_outcomes.artifact_imported.load(
          std::memory_order_relaxed)) +
      " artifact-probe-hits=" +
      std::to_string(precompile_outcomes.artifact_probe_hits.load(
          std::memory_order_relaxed)) +
      " shared-slab-hits=" +
      std::to_string(precompile_outcomes.shared_slab_hits.load(
          std::memory_order_relaxed)) +
      " deferred=" +
      std::to_string(precompile_outcomes.deferred.load(
          std::memory_order_relaxed)) +
      " unstable=" +
      std::to_string(precompile_outcomes.unstable.load(
          std::memory_order_relaxed)) +
      " cache-full=" +
      std::to_string(precompile_outcomes.cache_full.load(
          std::memory_order_relaxed)) +
      " failed=" +
      std::to_string(precompile_outcomes.failed.load(
          std::memory_order_relaxed)) +
      " deadline-stops=" +
      std::to_string(precompile_outcomes.deadline_stops.load(
          std::memory_order_relaxed)));
  const auto schedule_skip = [&precompile_schedule_skips](
                                 PrecompileScheduleSkip reason) {
    return std::to_string(
        precompile_schedule_skips[static_cast<std::size_t>(reason)]);
  };
  output.line(
      "[precompile-schedule] no-runtime=" +
      schedule_skip(PrecompileScheduleSkip::NoRuntime) +
      " task-busy=" + schedule_skip(PrecompileScheduleSkip::TaskBusy) +
      " no-phase=" + schedule_skip(PrecompileScheduleSkip::NoPhase) +
      " memory-pressure=" +
      schedule_skip(PrecompileScheduleSkip::MemoryPressure) +
      " display-quiet=" +
      schedule_skip(PrecompileScheduleSkip::DisplayQuiet) +
      " guest-not-idle=" +
      schedule_skip(PrecompileScheduleSkip::GuestNotIdle) +
      " deadline-reserve=" +
      schedule_skip(PrecompileScheduleSkip::DeadlineReserve) +
      " zero-budget=" + schedule_skip(PrecompileScheduleSkip::ZeroBudget) +
      " host-rejected=" +
      schedule_skip(PrecompileScheduleSkip::HostRejected));
  if (catalog_refresh_events != 0 || catalog_refresh_count != 0 ||
      catalog_refresh_scheduled != 0 || catalog_refresh_rejected != 0) {
    output.line(
        "[catalog] mutation-events=" +
        std::to_string(catalog_refresh_events) + " refreshes=" +
        std::to_string(catalog_refresh_count) + " offline-queue-pending=" +
        std::to_string(pending_catalog_compiles.size()) +
        " async-scheduled=" +
        std::to_string(catalog_refresh_scheduled) +
        " async-rejected=" +
        std::to_string(catalog_refresh_rejected) + " async-stale=" +
        std::to_string(catalog_refresh_stale));
  }
  if (catalog_loaded) {
    output.line(
        "[catalog] mapped-executable-ranges=" +
        std::to_string(catalog_mapped_executable_ranges) +
        " mapped-entry-hints=" +
        std::to_string(catalog_mapped_entry_hints));
    output.line(
        "[catalog] mapped-entry-phases=loader:" +
        std::to_string(catalog_mapped_entry_hints_by_phase[0]) +
        ",system-ui:" +
        std::to_string(catalog_mapped_entry_hints_by_phase[1]) +
        ",startup-service:" +
        std::to_string(catalog_mapped_entry_hints_by_phase[2]) +
        ",foreground-application:" +
        std::to_string(catalog_mapped_entry_hints_by_phase[3]) +
        ",remaining:" +
        std::to_string(catalog_mapped_entry_hints_by_phase[4]));
  }
  if (catalog_loaded && !executable_catalog.save(catalog_manifest)) {
    output.line("[catalog] manifest-save=failed");
  }
  for (auto &runtime : runtimes) {
    runtime_index.erase(*runtime);
    runtime_reaper.retire(std::move(runtime));
  }
  runtimes.clear();
  runtime_reaper.finish();
  if (*activation != LockdownActivation::Preserve) {
    // Native lockdownd may publish its runtime decision back into data_ark
    // during boot. Reapply the explicit simulator profile only after every
    // Guest runtime has been retired, so the requested state persists for the
    // next launch without racing a live daemon.
    const auto shutdown_activation_result =
        apply_lockdown_profile(*rootfs, *activation, lockdown_profile);
    output.line(
        "[device-state] shutdown-reapply=" + activation_value +
        " path=" + shutdown_activation_result.path.string() +
        " changed=" +
        std::to_string(shutdown_activation_result.changed));
  }
  if (report_performance) {
    // Make the reported disk footprint include artifacts generated during the
    // run. The store still performs the same atomic save again at destruction.
    static_cast<void>(jit_artifacts->save());
    const auto artifact_stats = jit_artifacts->stats();
    output.line(
        "[perf-artifact] lookup=" + std::to_string(artifact_stats.lookups) +
        " memory-hit=" + std::to_string(artifact_stats.memory_hits) +
        " disk-hit=" + std::to_string(artifact_stats.disk_hits) +
        " disk-retry=" +
        std::to_string(artifact_stats.disk_read_retries) +
        " disk-wait=" + std::to_string(artifact_stats.disk_read_waits) +
        " miss=" + std::to_string(artifact_stats.misses) +
        " publish=" + std::to_string(artifact_stats.publish_calls) +
        " dedup=" +
        std::to_string(artifact_stats.deduplicated_publishes) +
        " disk-load=" +
        std::to_string(artifact_stats.disk_loaded_entries) +
        " evict=" + std::to_string(artifact_stats.evictions) +
        " compactions=" + std::to_string(artifact_stats.compactions) +
        " quota-evictions=" +
        std::to_string(artifact_stats.quota_evictions) +
        " boot-working-set=" +
        std::to_string(artifact_stats.boot_working_set_artifacts) +
        " writeback-enqueued=" +
        std::to_string(artifact_stats.writeback_enqueued) +
        " writeback-saved=" +
        std::to_string(artifact_stats.writeback_saved) +
        " writeback-dropped=" +
        std::to_string(artifact_stats.writeback_dropped) +
        " writeback-failures=" +
        std::to_string(artifact_stats.writeback_failures) +
        " writeback-cancellations=" +
        std::to_string(artifact_stats.writeback_cancellations) +
        " resident-bytes=" +
        std::to_string(artifact_stats.resident_bytes) +
        " writeback-pending-bytes=" +
        std::to_string(artifact_stats.writeback_pending_bytes) +
        " disk-bytes=" + std::to_string(artifact_stats.disk_bytes));
    const auto &validation = artifact_stats.validation_rejections;
    output.line(
        "[perf-artifact-validation] unavailable=" +
        std::to_string(validation[static_cast<std::size_t>(
            JitArtifactValidationRejection::Unavailable)]) +
        " no-exact-artifact=" +
        std::to_string(validation[static_cast<std::size_t>(
            JitArtifactValidationRejection::NoExactArtifact)]) +
        " empty-ir=" +
        std::to_string(validation[static_cast<std::size_t>(
            JitArtifactValidationRejection::EmptyIr)]) +
        " dependency-mismatch=" +
        std::to_string(validation[static_cast<std::size_t>(
            JitArtifactValidationRejection::DependencyMismatch)]) +
        " deserialize-failed=" +
        std::to_string(validation[static_cast<std::size_t>(
            JitArtifactValidationRejection::DeserializeFailed)]) +
        " descriptor-mismatch=" +
        std::to_string(validation[static_cast<std::size_t>(
            JitArtifactValidationRejection::DescriptorMismatch)]) +
        " exception=" +
        std::to_string(validation[static_cast<std::size_t>(
            JitArtifactValidationRejection::Exception)]));
    const auto graphics_resource_bytes =
        gles_renderer ? gles_renderer->resource_bytes() : 0U;
    std::uint64_t host_resource_total = stopped_guest.jit_code_cache_bytes;
    const auto add_host_resource = [&host_resource_total](std::uint64_t bytes) {
      host_resource_total =
          bytes > std::numeric_limits<std::uint64_t>::max() -
                  host_resource_total
              ? std::numeric_limits<std::uint64_t>::max()
              : host_resource_total + bytes;
    };
    add_host_resource(artifact_stats.resident_bytes);
    add_host_resource(artifact_stats.writeback_pending_bytes);
    add_host_resource(file_page_cache_bytes);
    add_host_resource(graphics_resource_bytes);
    output.line(
        "[perf-host-resources] jit-native-bytes=" +
        std::to_string(stopped_guest.jit_code_cache_bytes) +
        " artifact-resident-bytes=" +
        std::to_string(artifact_stats.resident_bytes) +
        " artifact-writeback-bytes=" +
        std::to_string(artifact_stats.writeback_pending_bytes) +
        " file-page-cache-bytes=" +
        std::to_string(file_page_cache_bytes) + " graphics-bytes=" +
        std::to_string(graphics_resource_bytes) + " total-bytes=" +
        std::to_string(host_resource_total));
    const auto host_memory = host_memory_snapshot();
    output.line(
        "[perf-host-memory] rss-bytes=" +
        std::to_string(host_memory.rss_bytes) + " rss-peak-bytes=" +
        std::to_string(host_memory.peak_rss_bytes) + " virtual-bytes=" +
        std::to_string(host_memory.virtual_bytes) + " mmap-file-bytes=" +
        std::to_string(host_memory.file_mapped_bytes));
    output.line(
        "[perf-file-cache] identity-queries=" +
        std::to_string(file_cache_stats.identity_queries) +
        " sha-computations=" +
        std::to_string(file_cache_stats.sha_computations) + " sha-bytes=" +
        std::to_string(file_cache_stats.sha_bytes) + " identity-hits=" +
        std::to_string(file_cache_stats.identity_hits) +
        " generation-invalidations=" +
        std::to_string(file_cache_stats.generation_invalidations));
    const auto snapshot_stats = immutable_snapshot_stats();
    output.line(
        "[perf-snapshots] entries=" + std::to_string(snapshot_stats.entries) +
        " bytes=" + std::to_string(snapshot_stats.bytes) +
        " runtime-hot-entries=" +
        std::to_string(snapshot_stats.runtime_hot_entries) +
        " runtime-hot-bytes=" +
        std::to_string(snapshot_stats.runtime_hot_bytes) +
        " catalog-scan-entries=" +
        std::to_string(snapshot_stats.catalog_scan_entries) +
        " catalog-scan-bytes=" +
        std::to_string(snapshot_stats.catalog_scan_bytes) +
        " budget-bytes=" + std::to_string(snapshot_stats.budget_bytes) +
        " catalog-scan-budget-bytes=" +
        std::to_string(snapshot_stats.catalog_scan_budget_bytes) +
        " hits=" + std::to_string(snapshot_stats.hits) +
        " evictions=" + std::to_string(snapshot_stats.evictions));
    // Preserve stopped-guest live/current values, then include Runtime
    // destructor latency measured by the reaper in the final snapshot.
    auto final_snapshot = performance_counters().snapshot();
    final_snapshot.jit_live_instances = stopped_guest.jit_live_instances;
    final_snapshot.jit_code_cache_bytes = stopped_guest.jit_code_cache_bytes;
    final_snapshot.jit_shared_reserved_bytes =
        stopped_guest.jit_shared_reserved_bytes;
    final_snapshot.jit_shared_committed_bytes =
        stopped_guest.jit_shared_committed_bytes;
    final_snapshot.jit_shared_used_bytes =
        stopped_guest.jit_shared_used_bytes;
    final_snapshot.jit_executor_local_bytes =
        stopped_guest.jit_executor_local_bytes;
    final_snapshot.jit_executor_local_peak_bytes =
        stopped_guest.jit_executor_local_peak_bytes;
    final_snapshot.jit_cache_slots = stopped_guest.jit_cache_slots;
    output.line(format_performance_summary(final_snapshot));
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2) {
      std::cerr << usage();
      return 2;
    }
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
      args.emplace_back(argv[i]);
    }
    auto output = make_output(args);
    const auto perf_summary = flag(args, "--perf-summary");
    performance_counters().reset(perf_summary);
    const auto perf_frame_content = flag(args, "--perf-frame-content");
    const auto perf_cpu_phases = flag(args, "--perf-cpu-phases");
    if (perf_frame_content && !perf_summary) {
      throw std::runtime_error{
          "--perf-frame-content requires --perf-summary"};
    }
    performance_counters().set_frame_content_diagnostics(
        perf_frame_content);
    if (perf_cpu_phases && !perf_summary) {
      throw std::runtime_error{
          "--perf-cpu-phases requires --perf-summary"};
    }
    performance_counters().set_cpu_source_diagnostics(perf_cpu_phases);
    const std::string_view command{argv[1]};
    try {
      if (command == "profile") {
        profile(args, *output);
      } else if (command == "inspect") {
        inspect(args, *output);
      } else if (command == "catalog") {
        catalog(args, *output);
      } else if (command == "firmware") {
        if (args.empty() || args.front() != "prepare") {
          throw std::runtime_error{"firmware requires the 'prepare' mode"};
        }
        firmware_prepare(
            std::vector<std::string>{args.begin() + 1, args.end()}, *output);
      } else if (command == "disasm") {
        disasm(args, *output);
      } else if (command == "smoke") {
        smoke(args, *output);
      } else if (command == "benchmark") {
        benchmark(args, *output);
      } else if (command == "boot") {
        boot(args, *output);
      } else {
        throw std::runtime_error{"unknown command: " + std::string{command}};
      }
    } catch (...) {
      shutdown_gles_renderer();
      if (perf_summary) {
        output->line(
            format_performance_summary(performance_counters().snapshot()));
      }
      throw;
    }
    shutdown_gles_renderer();
    if (perf_summary && command != "boot") {
      output->line(
          format_performance_summary(performance_counters().snapshot()));
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "ilemu: " << error.what() << '\n';
    return 1;
  }
}
