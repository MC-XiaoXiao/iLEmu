#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dynarmic/interface/A32/disassembler.h>

#include "ilemu/address_space.hpp"
#include "ilemu/baseband_replay.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/device_profile.hpp"
#include "ilemu/display.hpp"
#include "ilemu/frame_file_presenter.hpp"
#include "ilemu/gdb_rsp.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/live_control.hpp"
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
constexpr std::size_t arm_thumb_breakpoint_size = 2;
constexpr std::size_t arm_breakpoint_size = 4;
// Host input, network completion, and display polling do not need a 1 kHz
// wakeup rate. Four milliseconds stays well below one 60 Hz frame while
// avoiding repeated full runtime scans between the same Guest deadline.
constexpr auto interactive_maximum_sleep = std::chrono::milliseconds{4};

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

struct Runtime {
  std::unique_ptr<AddressSpace> memory;
  std::unique_ptr<CpuCluster> cpus;
  std::unique_ptr<CompatibilityKernel> kernel;
  std::vector<bool> allocated;
  std::optional<PendingExec> pending_exec;
  bool fresh_spawn_address_space{};

  ~Runtime() {
    PerformanceLatencyScope latency{PerfLatencyKind::RuntimeDestructor};
    pending_exec.reset();
    std::vector<bool>{}.swap(allocated);
    kernel.reset();
    cpus.reset();
    memory.reset();
  }
};

class RuntimeReaper {
public:
  RuntimeReaper() : worker_{[this] { worker_loop(); }} {}
  RuntimeReaper(const RuntimeReaper &) = delete;
  RuntimeReaper &operator=(const RuntimeReaper &) = delete;

  ~RuntimeReaper() { finish(); }

  void retire(std::unique_ptr<Runtime> runtime) {
    if (!runtime)
      return;
    {
      std::lock_guard lock{mutex_};
      if (stopping_)
        throw std::logic_error{"cannot retire a Runtime after reaper stop"};
      pending_.push_back(std::move(runtime));
    }
    work_available_.notify_one();
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
      auto runtime = std::move(pending_.front());
      pending_.pop_front();
      active_ = true;
      lock.unlock();
      runtime.reset();
      lock.lock();
      active_ = false;
      idle_.notify_all();
    }
    idle_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable idle_;
  std::deque<std::unique_ptr<Runtime>> pending_;
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
  bool single_step{};
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
              : prepared.cpu->run(prepared.tick_budget,
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
         "  ilemu profile [--output FILE]\n"
         "  ilemu inspect --rootfs DIR [--binary /sbin/launchd] "
         "[--symbols SUBSTRING] [--output FILE]\n"
         "  ilemu disasm --rootfs DIR --binary PATH "
         "(--symbol NAME | --address ADDR) [--count N] [--thumb]\n"
         "  ilemu boot --rootfs DIR [--binary /sbin/launchd] [--ticks N] "
         "[--cores N] [--jit-cache-mib 8..128] "
         "[--watch-address ADDR] [--gdb PORT] "
         "[--display headless|sdl] [--network isolated|loopback|host] "
         "[--gles-backend auto|software|vulkan] [--gpu] "
         "[--display-size WIDTHxHEIGHT] "
         "[--activation activated|unactivated|preserve] "
         "[--frame-output FILE] [--touch-replay FILE] [--control-stdin] "
         "[--baseband-input FILE] [--baseband-output FILE] "
         "[--perf-summary] [--output FILE]\n"
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

void profile(Output &output) {
  const auto &device = DeviceProfile::default_profile();
  std::ostringstream text;
  text << "product: " << device.product_type << '\n'
       << "board: " << device.board_config << '\n'
       << "model_number: " << device.model_number << '\n'
       << "soc: " << device.soc << '\n'
       << "cpu: " << device.cpu_core << " (" << device.instruction_set << ")\n"
       << "cpu_hz: " << device.cpu_hz << '\n'
       << "ram_bytes: " << device.ram_bytes << '\n'
       << "physical_cpu_count: " << device.physical_cpu_count << '\n'
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
  const auto image = MachOImage::parse(host_path);

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
  const auto image =
      MachOImage::parse(std::filesystem::path{*rootfs} / relative);
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

  CpuCluster cluster{core_count, memory};
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
    text << "mode: stress/dev; execution-slot LDREX state follows the host "
            "slot and process-local ExclusiveMonitor does not model "
            "cross-process shared-page atomics\n";
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
  const auto rootfs = option(args, "--rootfs");
  if (!rootfs) {
    throw std::runtime_error{"boot requires --rootfs"};
  }
  const auto gles_backend = parse_gles_backend(args);
  configure_gles_pipeline_cache(
      std::filesystem::path{*rootfs} / "var/db/ilegacysim" /
      "vulkan-pipeline-cache.bin");
  configure_gles_backend(gles_backend);
  const auto binary = option(args, "--binary").value_or("/sbin/launchd");
  auto device = DeviceProfile::default_profile();
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
  const auto activation_result = apply_lockdown_profile(*rootfs, *activation);
  output.line("[device-state] activation=" + activation_value +
              " path=" + activation_result.path.string() +
              " changed=" + std::to_string(activation_result.changed));
  const auto activation_override =
      *activation == LockdownActivation::Preserve
          ? std::optional<bool>{}
          : std::optional<bool>{
                *activation == LockdownActivation::Activated};
  const auto ticks_option = option(args, "--ticks");
  const auto bounded_execution = ticks_option.has_value();
  const auto ticks = ticks_option ? std::stoull(*ticks_option)
                                  : std::numeric_limits<std::uint64_t>::max();
  const auto default_processor_count =
      device.physical_cpu_count;
  const auto cpu_model =
      make_arm_cpu_model(device.cpu_model, device.cpu_hz);
  const auto guest_ticks_per_second =
      cpu_model->ticks_per_second();
  GuestTickClock guest_tick_clock{guest_ticks_per_second};
  const auto guest_processor_count = static_cast<std::size_t>(
      std::stoul(option(args, "--cores")
                     .value_or(std::to_string(default_processor_count))));
  if (guest_processor_count == 0 ||
      guest_processor_count > maximum_virtual_processors) {
    throw std::runtime_error{"--cores must be in the range 1.." +
                             std::to_string(maximum_virtual_processors)};
  }
  if (guest_processor_count > 1) {
    output.line(
        "[cpu] mode=stress/dev cores=" +
        std::to_string(guest_processor_count) +
        " warning=\"execution-slot LDREX state follows the host slot; "
        "process-local ExclusiveMonitor does not model cross-process "
        "shared-page atomics\"");
  }
  const auto configured_jit_code_cache_size =
      jit_code_cache_size(args);
  output.line("[jit] code-cache-mib=" +
              std::to_string(configured_jit_code_cache_size /
                             1024U / 1024U));
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
  std::vector<std::string> preferred_wifi_networks;
  if (*network_policy != HostNetworkPolicy::Isolated) {
    const auto network_preferences = ensure_airport_network_service(
        *rootfs, "en0", wifi_interface_mac_address,
        NetworkPreferencesIpv4{
            .address = virtual_network::client_address,
            .netmask = virtual_network::netmask,
            .gateway = virtual_network::gateway_address,
            .dns_servers = {virtual_network::dns_proxy_address},
        });
    preferred_wifi_networks =
        network_preferences.preferred_wifi_networks;
    output.line(
        "[device-state] airport-service=" +
        (network_preferences.service_identifier.empty()
             ? std::string{"unavailable"}
             : network_preferences.service_identifier) +
        " path=" + network_preferences.path.string() +
        " supported=" + std::to_string(network_preferences.supported) +
        " changed=" + std::to_string(network_preferences.changed));
  }
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
  const auto baseband_input =
      baseband_input_path
          ? bsd::baseband_device::load_replay_file(*baseband_input_path)
          : std::vector<std::byte>{};

  auto initial_memory = std::make_unique<AddressSpace>();
  initial_memory->set_parallel_access(guest_processor_count > 1);
  ProcessLoader loader{*rootfs, *initial_memory};
  std::vector<std::string> initial_environment{
      "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/var/root",
      "SHELL=/bin/sh"};
  auto process = loader.load(binary, {}, initial_environment);
  RuntimeReaper runtime_reaper;
  std::vector<std::unique_ptr<Runtime>> runtimes;
  auto initial = std::make_unique<Runtime>();
  initial->memory = std::move(initial_memory);
  initial->cpus = std::make_unique<CpuCluster>(
      initial_guest_thread_slots, maximum_guest_threads, *initial->memory,
      guest_processor_count, *cpu_model);
  initial->cpus->set_jit_code_cache_size(
      configured_jit_code_cache_size);
  initial->kernel =
      std::make_unique<CompatibilityKernel>(*initial->memory, output, *rootfs,
                                            device, activation_override);
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
  initial->kernel->enqueue_baseband_input(baseband_input);
  if (baseband_input_path) {
    output.line("[baseband] replay input=" + *baseband_input_path +
                " bytes=" + std::to_string(baseband_input.size()));
  }
  if (sdl_display) {
    initial->kernel->set_display_presenter(
        [backend = sdl_display.get()](const DisplayFrame &frame) {
          backend->present(frame);
        });
  } else if (frame_file_presenter) {
    initial->kernel->set_display_presenter(
        [backend = frame_file_presenter.get(),
         &output](const DisplayFrame &frame) {
          backend->present(frame);
          const auto pixels =
              frame.pixels.empty() && frame.read_pixels
                  ? frame.read_pixels()
                  : frame.pixels;
          const auto visible = std::count_if(
              pixels.begin(), pixels.end(),
              [](std::uint32_t pixel) { return (pixel & 0x00ffffffU) != 0; });
          output.line("[display] frame=" + std::to_string(frame.sequence) +
                      " visible-pixels=" + std::to_string(visible));
        });
  }
  initial->allocated.assign(initial_guest_thread_slots, false);
  Runtime *initial_runtime = initial.get();
  runtimes.push_back(std::move(initial));
  initial_runtime->kernel->set_preferred_wifi_networks(
      preferred_wifi_networks);
  BootGdbTarget debug_target{runtimes};
  XnuScheduler scheduler{
      guest_ticks_per_second /
          xnu792::scheduler::default_preemption_rate,
      guest_ticks_per_second /
          xnu792::scheduler::scheduler_ticks_per_second,
      guest_processor_count};
  std::optional<XnuThreadId> last_serial_thread;

  std::uint32_t next_pid = 2;
  std::size_t watchpoint_trace_count = 0;
  std::mutex watchpoint_mutex;
  std::function<void(Runtime &)> configure_runtime;
  configure_runtime = [&](Runtime &runtime) {
    auto *runtime_ptr = &runtime;
    runtime.kernel->set_host_network_policy(*network_policy);
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
        [runtime_ptr, &scheduler](std::uint32_t pid, std::size_t processor) {
          if (pid != runtime_ptr->kernel->process().pid ||
              processor >= runtime_ptr->allocated.size() ||
              !runtime_ptr->allocated[processor] ||
              !scheduler.remove_thread(
                  XnuThreadId{pid, static_cast<std::uint32_t>(processor)})) {
            return false;
          }
          runtime_ptr->allocated[processor] = false;
          return true;
        });
    runtime.kernel->set_thread_state_query(
        [&runtimes](std::uint32_t pid, std::uint32_t slot, std::uint32_t flavor)
            -> std::optional<darwin::arm_thread::GeneralState> {
          if (flavor != darwin::arm_thread::general_state_flavor) {
            return std::nullopt;
          }
          const auto runtime = std::find_if(
              runtimes.begin(), runtimes.end(), [pid](const auto &candidate) {
                return candidate->kernel->process().pid == pid;
              });
          if (runtime == runtimes.end() || slot >= (*runtime)->cpus->size() ||
              slot >= (*runtime)->allocated.size() ||
              !(*runtime)->allocated[slot]) {
            return std::nullopt;
          }
          const auto &thread = (*runtime)->cpus->cpu(slot);
          darwin::arm_thread::GeneralState state{};
          std::copy(thread.registers().begin(), thread.registers().end(),
                    state.begin());
          state[darwin::arm_thread::cpsr_index] = thread.cpsr();
          return state;
        });
    runtime.kernel->set_thread_state_update_handler(
        [&runtimes](std::uint32_t pid, std::uint32_t slot,
                    const darwin::arm_thread::GeneralState &state) {
          const auto runtime = std::find_if(
              runtimes.begin(), runtimes.end(), [pid](const auto &candidate) {
                return candidate->kernel->process().pid == pid;
              });
          if (runtime == runtimes.end() || slot >= (*runtime)->cpus->size() ||
              slot >= (*runtime)->allocated.size() ||
              !(*runtime)->allocated[slot]) {
            return false;
          }
          auto &thread = (*runtime)->cpus->cpu(slot);
          std::copy_n(state.begin(), thread.registers().size(),
                      thread.registers().begin());
          thread.set_cpsr(state[darwin::arm_thread::cpsr_index] | 0x10U);
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
    const auto create_child_runtime =
        [&, runtime_ptr](Cpu *parent_cpu,
                         CompatibilityKernel::ProcessInheritance inheritance)
        -> std::optional<std::uint32_t> {
          const auto child_pid = next_pid++;
          auto child = std::make_unique<Runtime>();
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
                *child->memory, guest_processor_count, *cpu_model);
            child->cpus->set_jit_code_cache_size(
                configured_jit_code_cache_size);
          }
          {
            PerformanceLatencyScope latency{
                PerfLatencyKind::ProcessCreateKernel};
            child->kernel = std::make_unique<CompatibilityKernel>(
                *child->memory, output, *rootfs, device, activation_override);
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
          ProcessLoader validator{*rootfs, *runtime_ptr->memory};
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
          const auto child = std::find_if(
              runtimes.begin(), runtimes.end(),
              [child_pid](const auto &candidate) {
                return candidate->kernel->process().pid == child_pid;
              });
          if (child == runtimes.end())
            return false;

          auto &child_runtime = **child;
          try {
            debug_target.notify_exec(child_pid);
            if (!child_runtime.fresh_spawn_address_space) {
              PerformanceLatencyScope latency{
                  PerfLatencyKind::SpawnMemoryClear};
              child_runtime.memory->clear();
            }
            LoadedProcess loaded;
            {
              PerformanceLatencyScope latency{PerfLatencyKind::SpawnImageLoad};
              ProcessLoader loader{*rootfs, *child_runtime.memory};
              loaded = loader.load(path, std::move(arguments), environment);
            }
            {
              PerformanceLatencyScope latency{
                  PerfLatencyKind::SpawnResetRuntime};
              child_runtime.kernel->set_process_arguments(loaded.arguments,
                                                          environment);
              child_runtime.kernel->set_process_image(path);
              child_runtime.kernel->prepare_exec(0);
              auto &child_cpu = child_runtime.cpus->cpu(0);
              child_cpu.reset();
              child_cpu.clear_cache();
              child_cpu.registers().fill(0);
              child_cpu.registers()[13] = loaded.stack_pointer;
              child_cpu.registers()[15] = loaded.entry_point;
              child_cpu.set_cpsr(0x10);
              child_runtime.kernel->install_main_image_hle(
                  child_cpu, loaded.executable_path);
            }
            child_runtime.fresh_spawn_address_space = false;
            if (start_suspended) {
              static_cast<void>(scheduler.block(XnuThreadId{child_pid, 0}));
            }
          } catch (const std::exception &error) {
            output.line("[process] spawn exec failed pid=" +
                        std::to_string(child_pid) + " path=" + path +
                        " error=" + error.what());
            child_runtime.kernel->exit_process(127);
            scheduler.remove_process(child_pid);
            std::fill(child_runtime.allocated.begin(),
                      child_runtime.allocated.end(), false);
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
        [&runtimes, &scheduler](std::uint32_t target_pid,
                                std::uint32_t signal) {
          for (auto &target : runtimes) {
            if (target->kernel->process().pid != target_pid)
              continue;
            const auto error = target->kernel->deliver_signal(signal);
            if (error == 0 && target->kernel->process().exited) {
              scheduler.remove_process(target_pid);
            }
            return error;
          }
          return darwin::error::no_such_process;
        });
    runtime.kernel->set_wait_child_handler(
        [runtime_ptr, &runtimes](std::int32_t target_pid, bool reap) {
          CompatibilityKernel::WaitChildResult result;
          for (auto &child : runtimes) {
            const auto &child_process = child->kernel->process();
            if (child_process.reaped ||
                child_process.parent_pid !=
                    runtime_ptr->kernel->process().pid ||
                (target_pid != -1 &&
                 static_cast<std::uint32_t>(target_pid) !=
                     child_process.pid)) {
              continue;
            }
            result.has_child = true;
            if (!child_process.exited) {
              continue;
            }
            result.child_pid = child_process.pid;
            result.status = child_process.termination_signal != 0
                                ? child_process.termination_signal & 0x7fU
                                : (child_process.exit_status & 0xffU) << 8U;
            if (reap) {
              static_cast<void>(child->kernel->reap_process());
            }
            break;
          }
          return result;
        });
    runtime.kernel->set_task_memory_region_query(
        [&runtimes](std::uint32_t pid, std::uint32_t address)
            -> std::optional<AddressSpace::MappingRegion> {
          const auto runtime = std::find_if(
              runtimes.begin(), runtimes.end(), [pid](const auto &candidate) {
                return candidate->kernel->process().pid == pid;
              });
          if (runtime == runtimes.end())
            return std::nullopt;
          return (*runtime)->memory->mapping_region_at_or_after(address);
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
  std::optional<std::string> display_performance_window;
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
            output.line("[control] display wake requested before gesture");
          }
          live_touch_scheduler.schedule(command.gesture);
          output.line(
              "[control] gesture=" + command.message +
              " scheduled events=" + std::to_string(command.gesture.size()));
          break;
        case LiveControlCommandKind::Wake:
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{SystemButton::Home, SystemButtonPhase::Down});
          initial_runtime->kernel->enqueue_system_button(
              SystemButtonInput{SystemButton::Home, SystemButtonPhase::Up});
          output.line("[control] display wake requested");
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
              display_performance_window = command.message;
              output.line("[control] perf-begin label=" + command.message);
            }
          }
          break;
        case LiveControlCommandKind::PerfEnd:
          if (!display_performance_window) {
            output.line("[control] error: no active perf window");
          } else {
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
            display_performance_window.reset();
          }
          break;
        case LiveControlCommandKind::Status: {
          const auto frame = initial_runtime->kernel->display_snapshot();
          const auto active_process =
              initial_runtime->kernel->active_client_process_id();
          output.line(
              "[control] status frame=" + std::to_string(frame.sequence) +
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
                      "wake; lock; volume-up; volume-down; snapshot PATH; "
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
          // The host is currently slower than the calibrated guest clock.
          // Rebase pacing at the achieved virtual time so this deficit is not
          // injected later as one large timer jump when the guest next idles.
          realtime_pacer.emplace(current_time);
        }
      }
      const auto delay = realtime_pacer->delay_until(
          initial_runtime->kernel->current_absolute_time());
      if (delay > std::chrono::nanoseconds::zero()) {
        std::this_thread::sleep_for(std::min(
            delay, std::chrono::duration_cast<std::chrono::nanoseconds>(
                       interactive_maximum_sleep)));
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
            scheduling_info->state != XnuThreadState::Waiting) {
          continue;
        }
        auto &waiting_cpu = runtime->cpus->cpu(processor);
        if (runtime->kernel->deliver_pending_io(waiting_cpu) ||
            runtime->kernel->deliver_pending_mach(waiting_cpu)) {
          static_cast<void>(scheduler.make_runnable(thread));
        }
      }
    }
    for (auto &parent : runtimes) {
      const auto pending_waits = parent->kernel->pending_waits();
      for (const auto &[processor, pending] : pending_waits) {
        bool has_waitable_child = false;
        bool completed = false;
        for (auto &child : runtimes) {
          const auto &child_process = child->kernel->process();
          if (child_process.reaped ||
              child_process.parent_pid != parent->kernel->process().pid ||
              (pending.target_pid != -1 &&
               static_cast<std::uint32_t>(pending.target_pid) !=
                   child_process.pid)) {
            continue;
          }
          has_waitable_child = true;
          if (!child_process.exited)
            continue;
          const auto wait_status =
              child_process.termination_signal != 0
                  ? child_process.termination_signal & 0x7fU
                  : (child_process.exit_status & 0xffU) << 8U;
          if (parent->kernel->complete_wait(parent->cpus->cpu(processor),
                                            child_process.pid, wait_status)) {
            static_cast<void>(child->kernel->reap_process());
            static_cast<void>(scheduler.make_runnable(
                XnuThreadId{parent->kernel->process().pid,
                            static_cast<std::uint32_t>(processor)}));
            completed = true;
          }
          break;
        }
        if (!completed && !has_waitable_child) {
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
          (*runtime)->kernel->process().reaped) {
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
        scheduled_batch.push_back(*scheduled);
        if (bounded_execution) {
          reservable_ticks -=
              std::min(reservable_ticks, scheduled->tick_budget);
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
          debug_request && debug_request->kind == GdbResumeKind::Step,
      });
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
    if (prepared_slices.size() == 1) {
      GuestSliceWorkerPool::execute(prepared_slices.front());
    } else if (!prepared_slices.empty()) {
      guest_slice_workers->run(prepared_slices);
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
      if (guest_processor_count > 1 && result.svc) {
        runtime.kernel->dispatch(cpu, *result.svc);
        // UserDefined2 stopped the Dynarmic worker at the SVC. Only
        // the reason explicitly requested by the serial kernel
        // dispatch represents the guest thread's scheduler state.
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
        debug_target.notify_exec(runtime.kernel->process().pid);
        runtime.memory->clear();
        ProcessLoader exec_loader{*rootfs, *runtime.memory};
        auto loaded =
            exec_loader.load(pending.path, std::move(pending.arguments),
                             pending.environment);
        runtime.kernel->set_process_arguments(loaded.arguments,
                                              pending.environment);
        runtime.kernel->set_process_image(pending.path);
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
        static_cast<void>(scheduler.complete_slice(
            scheduled->thread, result.ticks_consumed,
            XnuSliceCompletion::Terminate, XnuTimeAccounting::Deferred));
        scheduler.remove_process(runtime.kernel->process().pid);
        std::fill(runtime.allocated.begin(), runtime.allocated.end(), false);
        runtime.allocated[pending.processor] = true;
        static_cast<void>(scheduler.register_thread(
            XnuThreadId{runtime.kernel->process().pid,
                        static_cast<std::uint32_t>(pending.processor)},
            runtime.kernel->process().thread_base_priority));
        scheduler_completed = true;
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
      // not unlock or swap that front buffer. Refresh each process-local
      // surface after advancing virtual display time. Only the process that
      // owns surface ID 0x100 performs any pixel work.
      for (auto &runtime : runtimes) {
        if (!runtime->kernel->process().exited) {
          static_cast<void>(runtime->kernel->refresh_display_scanout());
        }
      }
    }
    if (!ran_thread) {
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
        const auto deadline = runtime->kernel->next_timer_deadline();
        if (deadline && (!next_deadline || *deadline < *next_deadline)) {
          next_deadline = deadline;
        }
      }
      if (next_deadline) {
        if (realtime_pacer) {
          const auto delay = realtime_pacer->delay_until(*next_deadline);
          if (delay > std::chrono::nanoseconds::zero()) {
            std::this_thread::sleep_for(std::min(
                delay, std::chrono::duration_cast<std::chrono::nanoseconds>(
                           interactive_maximum_sleep)));
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
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        continue;
      }
      if (bounded_execution)
        break;
      // An interactive emulator remains alive while every guest thread
      // is blocked: SDL input, GDB interrupt, and future host-network
      // completions may still produce a wakeup. Avoid a busy-spin while
      // retaining a responsive event loop.
      std::this_thread::sleep_for(interactive_maximum_sleep);
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
  if (baseband_output_path) {
    const auto captured = initial_runtime->kernel->take_baseband_output();
    bsd::baseband_device::write_capture_file(*baseband_output_path, captured);
    output.line("[baseband] capture output=" + *baseband_output_path +
                " bytes=" + std::to_string(captured.size()));
  }
  const auto report_performance = flag(args, "--perf-summary");
  if (sdl_display)
    sdl_display->flush_presentation();
  const auto stopped_guest = report_performance
                                 ? performance_counters().snapshot()
                                 : PerformanceSnapshot{};
  for (auto &runtime : runtimes)
    runtime_reaper.retire(std::move(runtime));
  runtimes.clear();
  runtime_reaper.finish();
  if (report_performance) {
    // Preserve stopped-guest live/current values, then include Runtime
    // destructor latency measured by the reaper in the final snapshot.
    auto final_snapshot = performance_counters().snapshot();
    final_snapshot.jit_live_instances = stopped_guest.jit_live_instances;
    final_snapshot.jit_code_cache_bytes = stopped_guest.jit_code_cache_bytes;
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
    const std::string_view command{argv[1]};
    try {
      if (command == "profile") {
        profile(*output);
      } else if (command == "inspect") {
        inspect(args, *output);
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
