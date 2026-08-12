#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/xattr.h>

#include "ilemu/address_space.hpp"
#include "ilemu/apple80211_hle.hpp"
#include "ilemu/clock_mig_ids.hpp"
#include "ilemu/clock_reply_mig_ids.hpp"
#include "ilemu/core_surface_abi.hpp"
#include "ilemu/core_surface_hle.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/device_mig_ids.hpp"
#include "ilemu/display.hpp"
#include "ilemu/dnssd_ipc_abi.hpp"
#include "ilemu/gdb_rsp.hpp"
#include "ilemu/gles_abi.hpp"
#include "ilemu/hfs_metadata.hpp"
#include "ilemu/host_network.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_namespace.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_port_object.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/macho.hpp"
#include "ilemu/mbx2d_abi.hpp"
#include "ilemu/mbx2d_hle.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/mobile_framebuffer_hle.hpp"
#include "ilemu/opengles_hle.hpp"
#include "ilemu/surface_store.hpp"
#include "ilemu/system_configuration_mig_ids.hpp"
#include "ilemu/userland_hle.hpp"
#include "ilemu/virtual_network.hpp"
#include "ilemu/wifi_state.hpp"
#include "ilemu/xnu_mig_adapter.hpp"
#include "ilemu/xnu_scheduler.hpp"

#include "suite.hpp"
#include "test_support.hpp"

namespace {

using namespace ilemu;
using ilemu::test::require;

void address_space_test() {
  AddressSpace memory;
  require(
      memory.map(0x1fff, 4, MemoryPermission::Read | MemoryPermission::Write),
      "cross-page map failed");
  require(memory.write32(0x1fff, 0x78563412U), "cross-page write failed");
  require(memory.read32(0x1fff) == std::optional<std::uint32_t>{0x78563412U},
          "cross-page little-endian read failed");
  require(memory.compare_exchange32(0x1fff, 0x78563412U, 0xaabbccddU),
          "atomic compare-exchange failed");
  require(memory.read32(0x1fff) == std::optional<std::uint32_t>{0xaabbccddU},
          "atomic result mismatch");

  constexpr std::uint32_t lazy_base = 0x8000;
  require(memory.map(lazy_base, 2U * AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "demand-zero map failed");
  require(memory.mapping_region_count() < memory.mapped_page_count(),
          "vm_map metadata did not coalesce adjacent pages");
  const auto resident_before_read = memory.resident_page_count();
  require(memory.read32(lazy_base + AddressSpace::page_size) == 0U &&
              memory.resident_page_count() == resident_before_read,
          "demand-zero read allocated a resident page");
  require(memory.write32(lazy_base, 0x11223344U),
          "demand-zero first write failed");
  auto child = memory.clone();
  require(memory.shared_page_count() != 0 && child->shared_page_count() != 0,
          "fork clone did not share resident page backing");
  require(child->write32(lazy_base, 0x55667788U),
          "copy-on-write child write failed");
  require(memory.read32(lazy_base) == 0x11223344U &&
              child->read32(lazy_base) == 0x55667788U,
          "copy-on-write did not isolate parent and child pages");

  AddressSpace file_memory;
  constexpr std::uint32_t file_base = 0x20000;
  std::ifstream source{__FILE__, std::ios::binary};
  char source_byte{};
  source.get(source_byte);
  require(source.good() && file_memory.map_file(
                               file_base, AddressSpace::page_size,
                               MemoryPermission::Read | MemoryPermission::Write,
                               __FILE__, 0),
          "shared file page map failed");
  auto file_child = file_memory.clone();
  require(file_memory.read8(file_base) ==
                  static_cast<std::uint8_t>(source_byte) &&
              file_memory.cached_file_page_count() == 1 &&
              file_child->cached_file_page_count() == 1,
          "forked address spaces did not reuse the file page cache");
  require(file_child->write8(
              file_base, static_cast<std::uint8_t>(
                             static_cast<std::uint8_t>(source_byte) ^ 0xffU)) &&
              file_memory.read8(file_base) ==
                  static_cast<std::uint8_t>(source_byte) &&
              file_memory.cached_file_mapping_count() == 1 &&
              file_child->cached_file_mapping_count() == 0,
          "file-backed copy-on-write modified the cached parent page");
}

void display_state_test() {
  DisplayState display;
  std::optional<DisplayFrame> presented;
  display.set_presenter([&](const DisplayFrame &frame) { presented = frame; });
  display.clear(0xff336699U);
  display.present();
  require(presented.has_value(), "display presenter was not called");
  require(presented->width == default_display_width &&
              presented->height == default_display_height,
          "display frame does not use the default device geometry");
  require(presented->sequence == 1 &&
              presented->pixels.size() ==
                  static_cast<std::size_t>(default_display_width) *
                      default_display_height,
          "display frame metadata mismatch");
  require(std::all_of(presented->pixels.begin(), presented->pixels.end(),
                      [](std::uint32_t pixel) { return pixel == 0xff336699U; }),
          "display clear did not update the framebuffer");
}

void cpu_cluster_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x4000;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write |
                         MemoryPermission::Execute),
          "code map failed");
  const std::array<std::byte, 8> code{
      std::byte{0x01}, std::byte{0x00}, std::byte{0x80}, std::byte{0xe2},
      std::byte{0x80}, std::byte{0x00}, std::byte{0x00}, std::byte{0xef},
  };
  require(memory.copy_in(base, code), "code load failed");

  CpuCluster cluster{2, memory};
  for (std::size_t index = 0; index < cluster.size(); ++index) {
    cluster.cpu(index).registers()[0] = static_cast<std::uint32_t>(10 + index);
    cluster.cpu(index).registers()[15] = base;
    cluster.cpu(index).set_cpsr(0x10);
  }
  const auto results = cluster.run_parallel(8);
  require(cluster.cpu(0).registers()[0] == 11, "cpu0 did not execute ARM code");
  require(cluster.cpu(1).registers()[0] == 12, "cpu1 did not execute ARM code");
  require(results[0].svc == std::optional<std::uint32_t>{0x80},
          "cpu0 SVC missing");
  require(results[1].svc == std::optional<std::uint32_t>{0x80},
          "cpu1 SVC missing");

  constexpr std::uint32_t atomic_code_base = 0x7000;
  constexpr std::uint32_t atomic_data_base = 0x8000;
  constexpr std::uint32_t lock_address = atomic_data_base;
  constexpr std::uint32_t counter_address = atomic_data_base + 4;
  constexpr std::uint32_t iterations_per_cpu = 10'000;
  constexpr std::uint64_t atomic_test_ticks = 2'000'000;
  const std::array<std::uint32_t, 12> atomic_code{
      0xe3a01001U, // mov r1, #1
      0xe1002091U, // retry: swp r2, r1, [r0]
      0xe3520000U, // cmp r2, #0
      0x1afffffcU, // bne retry
      0xe5943000U, // ldr r3, [r4]
      0xe2833001U, // add r3, r3, #1
      0xe5843000U, // str r3, [r4]
      0xe3a02000U, // mov r2, #0
      0xe5802000U, // str r2, [r0]
      0xe2555001U, // subs r5, r5, #1
      0x1afffff5U, // bne retry
      0xef000080U, // svc #0x80
  };
  require(memory.map(atomic_code_base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write |
                         MemoryPermission::Execute) &&
              memory.map(atomic_data_base, AddressSpace::page_size,
                         MemoryPermission::Read | MemoryPermission::Write),
          "atomic exchange maps failed");
  require(
      memory.copy_in(atomic_code_base, std::as_bytes(std::span{atomic_code})) &&
          memory.write32(lock_address, 0) && memory.write32(counter_address, 0),
      "atomic exchange setup failed");

  CpuCluster atomic_cluster{2, memory};
  for (std::size_t index = 0; index < atomic_cluster.size(); ++index) {
    auto &cpu = atomic_cluster.cpu(index);
    cpu.registers()[0] = lock_address;
    cpu.registers()[4] = counter_address;
    cpu.registers()[5] = iterations_per_cpu;
    cpu.registers()[15] = atomic_code_base;
    cpu.set_cpsr(0x10);
  }
  const auto atomic_results = atomic_cluster.run_parallel(atomic_test_ticks);
  require(memory.read32(lock_address) == std::optional<std::uint32_t>{0},
          "SWP spin lock was not released");
  require(memory.read32(counter_address) ==
              std::optional<std::uint32_t>{iterations_per_cpu * 2},
          "SWP did not serialize concurrent guest critical sections");
  require(std::ranges::all_of(atomic_results,
                              [](const CpuRunResult &result) {
                                return result.svc ==
                                       std::optional<std::uint32_t>{0x80};
                              }),
          "SWP regression program did not finish on both CPUs");
}

void cpu_deferred_svc_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x5000;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write |
                         MemoryPermission::Execute),
          "deferred SVC code map failed");
  require(memory.write32(base, 0xef000080U),
          "deferred SVC instruction write failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::uint32_t dispatched_immediate = 0;
  cpu.set_svc_handler([&](Cpu &, std::uint32_t immediate) {
    dispatched_immediate = immediate;
  });
  cpu.set_svc_dispatch_mode(SvcDispatchMode::Deferred);
  cpu.registers()[15] = base;
  cpu.set_cpsr(0x10);

  const auto result = cpu.run(4);
  require(result.svc == std::optional<std::uint32_t>{0x80},
          "deferred SVC immediate was not captured");
  require(Dynarmic::Has(result.reason, Dynarmic::HaltReason::UserDefined2),
          "deferred SVC did not stop the execution slice");
  require(dispatched_immediate == 0,
          "deferred SVC invoked the compatibility kernel on the worker");

  cpu.halt(Dynarmic::HaltReason::UserDefined5);
  require(Dynarmic::Has(cpu.consume_requested_halt_reason(),
                        Dynarmic::HaltReason::UserDefined5),
          "deferred kernel halt reason was not retained for serial commit");
  require(!Dynarmic::Has(cpu.consume_requested_halt_reason(),
                         Dynarmic::HaltReason::UserDefined5),
          "deferred kernel halt reason was not consumed exactly once");
}

void cpu_debug_breakpoint_test() {
  AddressSpace memory;
  constexpr std::uint32_t base = 0x6000;
  require(memory.map(base, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write |
                         MemoryPermission::Execute),
          "debug breakpoint code map failed");
  require(memory.write32(base, 0xe1200070U),
          "ARM BKPT instruction write failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  cpu.set_debug_breakpoints_enabled(true);
  cpu.registers()[15] = base;
  cpu.set_cpsr(0x10);
  const auto result = cpu.run(4);
  require(result.debug_breakpoint == std::optional<std::uint32_t>{base},
          "ARM BKPT did not produce a debugger stop");
  require(result.exception.empty(),
          "debug-enabled BKPT was reported as an ARM exception");
  require(cpu.registers()[15] == base,
          "debug breakpoint did not preserve the stopped PC");
}

void userland_hle_dispatch_test() {
  AddressSpace memory;
  constexpr std::uint32_t function_address = 0x7000;
  constexpr std::uint32_t return_address = function_address + 4;
  constexpr std::uint32_t stack_address = 0x9000;
  require(memory.map(function_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write |
                         MemoryPermission::Execute),
          "userspace HLE code map failed");
  require(memory.map(stack_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "userspace HLE stack map failed");
  require(memory.write32(function_address,
                         0xef000000U | userland_hle_svc_namespace | 1U),
          "userspace HLE SVC write failed");
  require(memory.write32(return_address, 0xef000080U),
          "userspace HLE return SVC write failed");
  require(memory.write32(stack_address, 0x55667788U),
          "userspace HLE stack argument write failed");

  std::ostringstream stream;
  Output output{stream};
  UserlandHleRegistry registry{memory, output};
  registry.record_loaded_image(
      "build/rootfs/Applications/Test.app/Test");
  require(registry.image_loaded_beneath("Applications/") &&
              !registry.image_loaded_beneath("System/Library/") &&
              !registry.image_loaded_beneath(""),
          "userspace HLE loaded-image directory classification failed");
  const auto data = registry.allocate_data(24, 16);
  const auto adjacent = registry.allocate_data(24, 16);
  require(data != 0 && (data & 15U) == 0 && adjacent == data + 32U &&
              memory.write32(data, 0x12345678U),
          "userspace HLE data allocator alignment or mapping failed");
  bool called = false;
  registry.register_function(
      "/usr/lib/Test.dylib", "_hleTest", [&](UserlandHleCall &call) {
        called = call.argument(0) == 0x11223344U &&
                 call.argument(4) == 0x55667788U && call.process_id() == 17 &&
                 call.symbol() == "_hleTest" &&
                 call.image_loaded_beneath("Applications/");
        call.set_return(0xa5a55a5aU);
      });

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  cpu.set_svc_handler([&](Cpu &source, std::uint32_t immediate) {
    if (registry.dispatch(source, 17, immediate))
      return;
    if (immediate == 0x80) {
      source.halt();
    }
  });
  cpu.registers()[0] = 0x11223344U;
  cpu.registers()[13] = stack_address;
  cpu.registers()[14] = return_address;
  cpu.registers()[15] = function_address;
  cpu.set_cpsr(0x10);
  const auto result = cpu.run(16);
  require(called, "userspace HLE did not receive ARM register/stack arguments");
  require(cpu.registers()[0] == 0xa5a55a5aU,
          "userspace HLE return value was lost");
  require(result.svc == std::optional<std::uint32_t>{0x80},
          "userspace HLE did not return through LR");
}

void layerkit_root_compatibility_test() {
  constexpr std::uint32_t context = 0x1000U;
  constexpr std::uint32_t detached_root = 0x2000U;
  constexpr std::uint32_t wrapper = 0x3000U;
  constexpr std::uint32_t window_root = 0x4000U;
  constexpr std::uint32_t layer_flags = 0x21220001U;

  LayerKitRootCompatibility compatibility;
  compatibility.set_layer_id(context, detached_root);
  require(compatibility.application_window_placement(
              context, detached_root, detached_root, layer_flags,
              std::bit_cast<std::uint32_t>(160.0F),
              std::bit_cast<std::uint32_t>(230.0F),
              std::bit_cast<std::uint32_t>(320.0F),
              std::bit_cast<std::uint32_t>(460.0F), 320U, 480U) ==
                  std::optional<LayerKitApplicationPlacement>{
                      {std::bit_cast<std::uint32_t>(250.0F), 0.0F, 20.0F,
                       20.0F}} &&
              compatibility.application_window_placement(
                  context, detached_root, detached_root, layer_flags,
                  std::bit_cast<std::uint32_t>(160.0F),
                  std::bit_cast<std::uint32_t>(250.0F),
                  std::bit_cast<std::uint32_t>(320.0F),
                  std::bit_cast<std::uint32_t>(460.0F), 320U, 480U) ==
                  std::optional<LayerKitApplicationPlacement>{
                      {std::bit_cast<std::uint32_t>(250.0F), 0.0F, 0.0F,
                       20.0F}} &&
              compatibility.application_window_placement(
                  context, detached_root, detached_root, layer_flags,
                  std::bit_cast<std::uint32_t>(384.0F),
                  std::bit_cast<std::uint32_t>(500.0F),
                  std::bit_cast<std::uint32_t>(768.0F),
                  std::bit_cast<std::uint32_t>(1000.0F), 768U, 1024U) ==
                  std::optional<LayerKitApplicationPlacement>{
                      {std::bit_cast<std::uint32_t>(524.0F), 0.0F, 24.0F,
                       24.0F}} &&
              compatibility.application_window_placement(
                  context, detached_root, detached_root, layer_flags,
                  std::bit_cast<std::uint32_t>(160.0F),
                  std::bit_cast<std::uint32_t>(240.0F),
                  std::bit_cast<std::uint32_t>(320.0F),
                  std::bit_cast<std::uint32_t>(480.0F), 320U, 480U) ==
                  std::optional<LayerKitApplicationPlacement>{
                      {std::bit_cast<std::uint32_t>(240.0F), 0.0F, 0.0F,
                       0.0F}} &&
              !compatibility.application_window_placement(
                  context, detached_root, wrapper, layer_flags,
                  std::bit_cast<std::uint32_t>(160.0F),
                  std::bit_cast<std::uint32_t>(208.0F),
                  std::bit_cast<std::uint32_t>(320.0F),
                  std::bit_cast<std::uint32_t>(416.0F), 320U, 480U),
          "LayerKit compatibility did not preserve the Purple application "
          "context inset");
  require(!compatibility.observe_commit(context, detached_root, window_root,
                                        layer_flags, 0U, {}, false),
          "LayerKit compatibility replaced a complete layer");
  const std::array children{window_root};
  require(!compatibility.observe_commit(context, detached_root, wrapper,
                                        0x20020001U, 2U, children, false),
          "LayerKit compatibility replaced the wrapper layer");
  constexpr std::uint32_t nested_parent = 0x4100U;
  constexpr std::uint32_t nested_child = 0x4200U;
  static_cast<void>(compatibility.observe_commit(
      context, detached_root, nested_child, layer_flags, 0U, {}, false));
  const std::array nested_children{nested_child};
  static_cast<void>(compatibility.observe_commit(context, detached_root,
                                                 nested_parent, layer_flags, 0U,
                                                 nested_children, false));
  static_cast<void>(compatibility.observe_commit(context, detached_root,
                                                 nested_parent, 0x20020001U, 2U,
                                                 nested_children, false));
  require(compatibility.observe_commit(context, detached_root, detached_root,
                                       0x20020001U, 0U, {}, false) ==
              std::optional<std::uint32_t>{wrapper},
          "LayerKit compatibility did not reconnect the window wrapper");
  require(!compatibility.observe_commit(context, wrapper, detached_root,
                                        0x20020001U, 0U, {}, false),
          "LayerKit compatibility redirected one context twice");

  constexpr std::uint32_t complete_wrapper_context = 0x7000U;
  constexpr std::uint32_t complete_wrapper_root = 0x7100U;
  constexpr std::uint32_t complete_wrapper = 0x7200U;
  constexpr std::uint32_t first_complete_child = 0x7300U;
  constexpr std::uint32_t second_complete_child = 0x7400U;
  compatibility.set_layer_id(complete_wrapper_context, complete_wrapper_root);
  static_cast<void>(compatibility.observe_commit(
      complete_wrapper_context, complete_wrapper_root, first_complete_child,
      layer_flags, 127U, {}, false));
  static_cast<void>(compatibility.observe_commit(
      complete_wrapper_context, complete_wrapper_root, second_complete_child,
      layer_flags, 127U, {}, false));
  const std::array complete_wrapper_children{first_complete_child,
                                             second_complete_child};
  static_cast<void>(compatibility.observe_commit(
      complete_wrapper_context, complete_wrapper_root, complete_wrapper,
      layer_flags, 0U, complete_wrapper_children, false));
  require(compatibility.observe_commit(
              complete_wrapper_context, complete_wrapper_root,
              complete_wrapper_root, 0x20020001U, 0U, {}, false) ==
              std::optional<std::uint32_t>{complete_wrapper},
          "LayerKit compatibility did not infer a unique complete wrapper");

  constexpr std::uint32_t complete_root_context = 0x9000U;
  constexpr std::uint32_t complete_root = 0x9100U;
  constexpr std::uint32_t complete_root_child = 0x9200U;
  compatibility.set_layer_id(complete_root_context, complete_root);
  static_cast<void>(compatibility.observe_commit(
      complete_root_context, complete_root, complete_root_child, layer_flags,
      127U, {}, false));
  const std::array complete_root_children{complete_root_child};
  static_cast<void>(compatibility.observe_commit(
      complete_root_context, complete_root, complete_root, layer_flags, 127U,
      complete_root_children, false));
  require(!compatibility.observe_commit(complete_root_context, complete_root,
                                        complete_root, 0x20020001U, 0U, {},
                                        false),
          "LayerKit compatibility replaced an already complete context root");

  constexpr std::uint32_t ambiguous_context = 0x8000U;
  constexpr std::uint32_t ambiguous_root = 0x8100U;
  compatibility.set_layer_id(ambiguous_context, ambiguous_root);
  static_cast<void>(compatibility.observe_commit(
      ambiguous_context, ambiguous_root, first_complete_child, layer_flags,
      127U, {}, false));
  static_cast<void>(compatibility.observe_commit(
      ambiguous_context, ambiguous_root, second_complete_child, layer_flags,
      127U, {}, false));
  require(!compatibility.observe_commit(ambiguous_context, ambiguous_root,
                                        ambiguous_root, 0x20020001U, 0U, {},
                                        false),
          "LayerKit compatibility guessed between ambiguous complete roots");

  constexpr std::uint32_t replacement_wrapper = 0x7500U;
  constexpr std::uint32_t replacement_child = 0x7600U;
  compatibility.set_layer_id(complete_wrapper_context, complete_wrapper_root);
  static_cast<void>(compatibility.observe_commit(
      complete_wrapper_context, complete_wrapper_root, replacement_child,
      layer_flags, 127U, {}, false));
  const std::array replacement_children{replacement_child};
  static_cast<void>(compatibility.observe_commit(
      complete_wrapper_context, complete_wrapper_root, replacement_wrapper,
      layer_flags, 0U, replacement_children, false));
  require(compatibility.observe_commit(
              complete_wrapper_context, complete_wrapper_root,
              complete_wrapper_root, 0x20020001U, 0U, {}, false) ==
              std::optional<std::uint32_t>{replacement_wrapper},
          "LayerKit compatibility retained the previous application graph");

  constexpr std::uint32_t next_root = 0x5000U;
  constexpr std::uint32_t next_window = 0x6000U;
  compatibility.set_layer_id(context, next_root);
  const std::array next_children{next_window};
  static_cast<void>(compatibility.observe_commit(
      context, next_root, wrapper, 0x20020001U, 2U, next_children, false));
  require(!compatibility.observe_commit(context, next_root, next_root,
                                        0x20020001U, 0U, {}, false),
          "LayerKit compatibility accepted an uncommitted window root");
  static_cast<void>(compatibility.observe_commit(
      context, next_root, next_window, layer_flags, 0U, {}, false));
  static_cast<void>(compatibility.observe_commit(
      context, next_root, wrapper, 0x20020001U, 2U, next_children, false));
  require(!compatibility.observe_commit(context, next_root, next_root,
                                        0x20020001U, 0U, {}, true) &&
              compatibility.observe_commit(context, next_root, next_root,
                                           0x20020001U, 0U, {}, false) ==
                  std::optional<std::uint32_t>{wrapper},
          "LayerKit compatibility ignored the root handle lifecycle");
}

void gdb_rsp_protocol_test() {
  class Target final : public GdbTarget {
  public:
    Target() {
      registers[0] = 0x12345678U;
      registers[gdb_arm_pc_register] = base;
      registers[gdb_arm_cpsr_register] = 0x10;
    }
    std::vector<GdbThreadId> threads() const override {
      return {{1, 1}, {2, 1}};
    }
    std::optional<GdbThreadId> current_thread() const override {
      return GdbThreadId{1, 1};
    }
    std::optional<GdbArmRegisters>
    read_registers(GdbThreadId thread) const override {
      return thread == GdbThreadId{1, 1}
                 ? std::optional<GdbArmRegisters>{registers}
                 : std::nullopt;
    }
    bool write_registers(GdbThreadId thread,
                         const GdbArmRegisters &values) override {
      if (thread != GdbThreadId{1, 1})
        return false;
      registers = values;
      return true;
    }
    std::optional<std::vector<std::byte>>
    read_memory(GdbThreadId thread, std::uint32_t address,
                std::size_t size) const override {
      if (thread != GdbThreadId{1, 1} || address < base ||
          address - base > memory.size() ||
          size > memory.size() - (address - base)) {
        return std::nullopt;
      }
      const auto offset = static_cast<std::size_t>(address - base);
      return std::vector<std::byte>{
          memory.begin() + static_cast<std::ptrdiff_t>(offset),
          memory.begin() + static_cast<std::ptrdiff_t>(offset + size)};
    }
    bool write_memory(GdbThreadId thread, std::uint32_t address,
                      std::span<const std::byte> bytes) override {
      if (thread != GdbThreadId{1, 1} || address < base ||
          address - base > memory.size() ||
          bytes.size() > memory.size() - (address - base)) {
        return false;
      }
      std::copy(bytes.begin(), bytes.end(),
                memory.begin() + static_cast<std::ptrdiff_t>(address - base));
      return true;
    }
    bool insert_software_breakpoint(GdbThreadId thread, std::uint32_t address,
                                    std::size_t kind) override {
      breakpoint = thread == GdbThreadId{1, 1} && address == base && kind == 4;
      return breakpoint;
    }
    bool remove_software_breakpoint(GdbThreadId thread, std::uint32_t address,
                                    std::size_t kind) override {
      const auto valid = breakpoint && thread == GdbThreadId{1, 1} &&
                         address == base && kind == 4;
      if (valid)
        breakpoint = false;
      return valid;
    }

    const std::uint32_t base{0x1000};
    GdbArmRegisters registers{};
    std::array<std::byte, 8> memory{
        std::byte{0x01}, std::byte{0x23}, std::byte{0x45}, std::byte{0x67},
        std::byte{0x89}, std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}};
    bool breakpoint{};
  } target;

  GdbRspProtocol protocol;
  protocol.set_stop({1, 1});
  const auto stop = protocol.handle("?", target);
  require(stop.response == std::optional<std::string>{"T05thread:p1.1;"},
          "GDB stop reply mismatch");
  const auto supported = protocol.handle("qSupported:multiprocess+", target);
  require(supported.response &&
              supported.response->find("multiprocess+") != std::string::npos,
          "GDB feature negotiation omitted multiprocess support");
  const auto threads = protocol.handle("qfThreadInfo", target);
  require(threads.response == std::optional<std::string>{"mp1.1,p2.1"},
          "GDB thread enumeration mismatch");
  const auto registers = protocol.handle("g", target);
  require(registers.response &&
              registers.response->size() == gdb_arm_core_register_count * 8U &&
              registers.response->starts_with("78563412"),
          "GDB register block encoding mismatch");
  require(protocol.handle("P0=04030201", target).response ==
                  std::optional<std::string>{"OK"} &&
              target.registers[0] == 0x01020304U,
          "GDB single-register write failed");
  require(protocol.handle("m1000,4", target).response ==
              std::optional<std::string>{"01234567"},
          "GDB memory read encoding mismatch");
  require(protocol.handle("M1000,2:aabb", target).response ==
                  std::optional<std::string>{"OK"} &&
              target.memory[0] == std::byte{0xaa} &&
              target.memory[1] == std::byte{0xbb},
          "GDB memory write failed");
  require(protocol.handle("Z0,1000,4", target).response ==
                  std::optional<std::string>{"OK"} &&
              target.breakpoint,
          "GDB software breakpoint insertion failed");
  require(protocol.handle("z0,1000,4", target).response ==
                  std::optional<std::string>{"OK"} &&
              !target.breakpoint,
          "GDB software breakpoint removal failed");
  const auto xml =
      protocol.handle("qXfer:features:read:target.xml:0,400", target);
  require(xml.response &&
              xml.response->find("<architecture>arm</architecture>") !=
                  std::string::npos,
          "GDB target description transfer failed");
  const auto step = protocol.handle("s", target);
  require(step.resume && step.resume->kind == GdbResumeKind::Step &&
              step.resume->thread == std::optional<GdbThreadId>{{1, 1}},
          "GDB single-step request was not decoded");
  require(protocol.handle("Hc-1", target).response ==
              std::optional<std::string>{"OK"},
          "GDB all-thread continue selection failed");
  const auto resume = protocol.handle("c", target);
  require(resume.resume && resume.resume->kind == GdbResumeKind::Continue &&
              !resume.resume->thread,
          "GDB all-thread continue request was not decoded");
  const auto no_ack = protocol.handle("QStartNoAckMode", target);
  require(no_ack.response == std::optional<std::string>{"OK"} &&
              no_ack.enable_no_ack,
          "GDB no-ack negotiation failed");
}

void syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t code_address = 0x8000;
  constexpr std::uint32_t text_address = 0x9000;
  require(memory.map(code_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write |
                         MemoryPermission::Execute),
          "syscall code map failed");
  require(memory.map(text_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "syscall data map failed");
  const std::array<std::uint32_t, 6> instructions{
      0xe3a0c004U, // mov r12, #4 (write)
      0xef000080U, // svc #0x80
      0xe3a00000U, // mov r0, #0
      0xe3a0c001U, // mov r12, #1 (exit)
      0xef000080U, // svc #0x80
      0xeafffffeU, // b .
  };
  std::array<std::byte, instructions.size() * 4> code{};
  for (std::size_t word = 0; word < instructions.size(); ++word) {
    for (std::size_t byte = 0; byte < 4; ++byte) {
      code[word * 4 + byte] =
          static_cast<std::byte>((instructions[word] >> (byte * 8U)) & 0xffU);
    }
  }
  const std::array<std::byte, 4> message{std::byte{'i'}, std::byte{'O'},
                                         std::byte{'S'}, std::byte{'\n'}};
  require(memory.copy_in(code_address, code), "syscall code copy failed");
  require(memory.copy_in(text_address, message), "syscall text copy failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};
  require(memory.mapped(0x40000000U),
          "iPhoneOS 1.0 commpage was not mapped at 0x40000000");
  kernel.attach(cpu);
  cpu.registers()[0] = 1;
  cpu.registers()[1] = text_address;
  cpu.registers()[2] = static_cast<std::uint32_t>(message.size());
  cpu.registers()[15] = code_address;
  cpu.set_cpsr(0x10);
  const auto result = cpu.run(64);
  require(!result.fault, "syscall execution memory faulted");
  require(stream.str().starts_with("iOS\n"),
          "write syscall did not reach output sink");
  require(kernel.process().exited && kernel.process().exit_status == 0,
          "exit syscall did not update process state");
}

void resource_limit_syscall_test() {
  AddressSpace parent_memory;
  AddressSpace child_memory;
  constexpr std::uint32_t base = 0x4d000;
  constexpr std::uint32_t input_limit = base;
  constexpr std::uint32_t output_limit = base + 0x40;
  require(
      parent_memory.map(base, AddressSpace::page_size,
                        MemoryPermission::Read | MemoryPermission::Write) &&
          child_memory.map(base, AddressSpace::page_size,
                           MemoryPermission::Read | MemoryPermission::Write),
      "rlimit test memory map failed");

  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu parent_cpu{0, parent_memory, monitor};
  Cpu child_cpu{1, child_memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel parent{parent_memory, output};
  CompatibilityKernel child{child_memory, output};

  constexpr auto posix_open_files =
      darwin::resource::open_files | darwin::resource::posix_flag;
  parent_cpu.registers()[0] = posix_open_files;
  parent_cpu.registers()[1] = input_limit;
  parent_cpu.registers()[12] = darwin::syscall::get_resource_limit;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0 &&
              parent_memory.read64(input_limit) ==
                  std::optional<std::uint64_t>{
                      darwin::resource::initial_open_files} &&
              parent_memory.read64(input_limit + sizeof(std::uint64_t)) ==
                  std::optional<std::uint64_t>{
                      darwin::resource::maximum_open_files},
          "initial XNU RLIMIT_NOFILE projection mismatch");

  // mDNSResponder first writes the unchanged pair back, then raises the
  // soft limit to its 10240-fd minimum while retaining the hard limit.
  parent_cpu.registers()[0] = posix_open_files;
  parent_cpu.registers()[1] = input_limit;
  parent_cpu.registers()[12] = darwin::syscall::set_resource_limit;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0 &&
              parent_memory.write64(input_limit,
                                    darwin::resource::maximum_open_files) &&
              parent_memory.write64(input_limit + sizeof(std::uint64_t),
                                    darwin::resource::maximum_open_files),
          "mDNSResponder unchanged setrlimit sequence failed");
  parent_cpu.registers()[0] = posix_open_files;
  parent_cpu.registers()[1] = input_limit;
  parent_cpu.registers()[12] = darwin::syscall::set_resource_limit;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0,
          "root could not raise RLIMIT_NOFILE to mDNS minimum");

  parent_cpu.registers()[0] = posix_open_files;
  parent_cpu.registers()[1] = output_limit;
  parent_cpu.registers()[12] = darwin::syscall::get_resource_limit;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 0 &&
              parent_memory.read64(output_limit) ==
                  std::optional<std::uint64_t>{
                      darwin::resource::maximum_open_files} &&
              parent_memory.read64(output_limit + sizeof(std::uint64_t)) ==
                  std::optional<std::uint64_t>{
                      darwin::resource::maximum_open_files},
          "raised RLIMIT_NOFILE did not persist");

  child.inherit_process_state(parent, 42);
  child_cpu.registers()[0] = darwin::resource::open_files;
  child_cpu.registers()[1] = output_limit;
  child_cpu.registers()[12] = darwin::syscall::get_resource_limit;
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == 0 &&
              child_memory.read64(output_limit) ==
                  std::optional<std::uint64_t>{
                      darwin::resource::maximum_open_files},
          "fork did not inherit resource limits");

  child.process().uid = 501;
  child.process().effective_uid = 501;
  require(child_memory.write64(input_limit, darwin::resource::infinity) &&
              child_memory.write64(input_limit + sizeof(std::uint64_t),
                                   darwin::resource::infinity),
          "non-root rlimit fixture setup failed");
  child_cpu.registers()[0] = darwin::resource::open_files;
  child_cpu.registers()[1] = input_limit;
  child_cpu.registers()[12] = darwin::syscall::set_resource_limit;
  child.dispatch(child_cpu, 0x80);
  require(child_cpu.registers()[0] == darwin::error::operation_not_permitted,
          "non-root process raised its hard resource limit");

  parent_cpu.registers()[0] = darwin::resource::limit_count;
  parent_cpu.registers()[1] = output_limit;
  parent_cpu.registers()[12] = darwin::syscall::get_resource_limit;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == darwin::error::invalid_argument,
          "getrlimit accepted an invalid resource selector");

  // XNU fdalloc and getdtablesize use the current RLIMIT_NOFILE rather than
  // the boot-time NOFILE value. Exercise the boundary on both sides of 256,
  // which is the exact transition mDNSResponder's startup raise depends on.
  constexpr std::uint64_t allocation_limit = 260;
  require(parent_memory.write64(input_limit, allocation_limit) &&
              parent_memory.write64(input_limit + sizeof(std::uint64_t),
                                    darwin::resource::maximum_open_files),
          "fd allocation limit fixture setup failed");
  parent_cpu.registers()[0] = darwin::resource::open_files;
  parent_cpu.registers()[1] = input_limit;
  parent_cpu.registers()[12] = darwin::syscall::set_resource_limit;
  parent.dispatch(parent_cpu, 0x80);
  parent_cpu.registers()[12] = darwin::syscall::get_descriptor_table_size;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == allocation_limit,
          "getdtablesize ignored raised RLIMIT_NOFILE");

  for (std::uint32_t expected = 3;
       expected < static_cast<std::uint32_t>(allocation_limit); ++expected) {
    parent_cpu.registers()[12] = darwin::syscall::kqueue;
    parent.dispatch(parent_cpu, 0x80);
    require(parent_cpu.registers()[0] == expected,
            "fd allocator did not cross the legacy 256 descriptor cap");
  }
  parent_cpu.registers()[12] = darwin::syscall::kqueue;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == 24,
          "fd allocator did not enforce current RLIMIT_NOFILE");

  parent_cpu.registers()[0] = 3;
  parent_cpu.registers()[1] = static_cast<std::uint32_t>(allocation_limit);
  parent_cpu.registers()[12] = darwin::syscall::duplicate_to;
  parent.dispatch(parent_cpu, 0x80);
  require(parent_cpu.registers()[0] == darwin::error::bad_file_descriptor,
          "dup2 accepted a destination at RLIMIT_NOFILE");
}

void validate_iokit_connect_method_reply_with_firmware(
    AddressSpace &memory, std::uint32_t reply_address) {
  const std::array candidates{
      std::filesystem::path{
          "build/rootfs/System/Library/Frameworks/IOKit.framework/IOKit"},
      std::filesystem::path{
          "rootfs/System/Library/Frameworks/IOKit.framework/IOKit"},
  };
  const auto path = std::find_if(
      candidates.begin(), candidates.end(),
      [](const auto &candidate) { return std::filesystem::exists(candidate); });
  if (path == candidates.end())
    return;

  const auto image = MachOImage::parse(*path);
  image.map_into(memory);
  const auto *validator =
      image.find_symbol("___MIG_check__Reply__io_connect_method_t");
  require(validator != nullptr,
          "firmware io_connect_method MIG validator is missing");

  // All three native-NDR checks in this exact firmware function resolve the
  // same non-lazy pointer. Decode its first PC-relative LDR/add pair instead
  // of embedding the image's relocated __DATA address in the test.
  constexpr std::uint32_t first_ndr_literal_load_offset = 0x88U;
  constexpr std::uint32_t following_pic_add_offset = 0x8cU;
  constexpr std::uint32_t arm_pc_pipeline_offset = 8U;
  const auto literal_load = validator->value + first_ndr_literal_load_offset;
  const auto load_instruction = memory.read32(literal_load);
  require(load_instruction && (*load_instruction & 0xfffff000U) == 0xe59f3000U,
          "firmware MIG validator NDR literal load changed");
  const auto literal_address =
      literal_load + arm_pc_pipeline_offset + (*load_instruction & 0x0fffU);
  const auto pic_delta = memory.read32(literal_address);
  require(pic_delta.has_value(),
          "firmware MIG validator PIC literal is unavailable");
  const auto ndr_pointer_slot = validator->value + following_pic_add_offset +
                                arm_pc_pipeline_offset + *pic_delta;

  constexpr std::uint32_t scratch = 0x60000U;
  constexpr std::uint32_t scratch_pages = 4;
  constexpr std::uint32_t ndr_record = scratch;
  constexpr std::uint32_t adjusted_scalar_pointer =
      scratch + AddressSpace::page_size;
  constexpr std::uint32_t adjusted_inband_pointer =
      adjusted_scalar_pointer + sizeof(std::uint32_t);
  constexpr std::uint32_t stack_top = scratch + 3U * AddressSpace::page_size;
  constexpr std::uint32_t return_address =
      scratch + 3U * AddressSpace::page_size;
  require(memory.map(scratch, scratch_pages * AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write |
                         MemoryPermission::Execute),
          "firmware MIG validator scratch mapping failed");
  require(memory.write32(ndr_record, 0) && memory.write32(ndr_record + 4U, 1) &&
              memory.write32(ndr_pointer_slot, ndr_record) &&
              memory.write32(return_address, 0xef000080U),
          "firmware MIG validator NDR/return setup failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  cpu.set_svc_handler([](Cpu &source, std::uint32_t immediate) {
    if (immediate == 0x80U)
      source.halt();
  });
  cpu.registers()[0] = reply_address;
  cpu.registers()[1] = adjusted_scalar_pointer;
  cpu.registers()[2] = adjusted_inband_pointer;
  cpu.registers()[13] = stack_top;
  cpu.registers()[14] = return_address;
  cpu.registers()[15] = validator->value;
  cpu.set_cpsr(0x10);
  const auto result = cpu.run(2'000);
  require(!result.fault && result.exception.empty() &&
              cpu.registers()[0] == 0 &&
              result.svc == std::optional<std::uint32_t>{0x80U},
          "real firmware rejected the generated io_connect_method reply");
}

void iokit_notification_test() {
  namespace device_mig = xnu792::mig::device;
  constexpr std::uint32_t mach_receive_invalid_data = 0x10004008U;
  AddressSpace memory;
  constexpr std::uint32_t message = 0x30000;
  require(memory.map(message, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "IOKit message map failed");
  KernelSharedState shared;
  ProcessContext process;
  process.pid = 23;
  shared.mach_namespaces.create_task(process.pid);
  const auto send_type = xnu792::ipc::type_mask(xnu792::ipc::Right::Send);
  for (const auto special : {process.task_port, process.io_master_port,
                             process.io_registry_options_port}) {
    require(shared.mach_namespaces.install(process.pid, special, special,
                                           send_type),
            "IOKit special port namespace installation failed");
  }
  shared.task_port_pids.emplace(process.task_port, process.pid);
  // Occupy the first task-local slot with an unrelated object so returned
  // IOKit names cannot accidentally equal their kernel object identifiers.
  require(shared.mach_namespaces.install(process.pid,
                                         xnu792::ipc::first_dynamic_name,
                                         0xdead0000U, send_type),
          "IOKit namespace collision setup failed");
  std::ostringstream stream;
  Output output{stream};

  constexpr std::string_view notification_type{"IOServiceFirstPublish\0", 22};
  std::array<std::byte, notification_type.size()> type_bytes{};
  for (std::size_t index = 0; index < notification_type.size(); ++index) {
    type_bytes[index] = static_cast<std::byte>(notification_type[index]);
  }
  const std::array<std::byte, 4> matching{std::byte{'m'}, std::byte{'a'},
                                          std::byte{'t'}, std::byte{'c'}};
  require(memory.write32(message + 24, 1),
          "IOKit descriptor count write failed");
  constexpr std::uint32_t notification_name = 0x100fc;
  constexpr std::uint32_t notification_object = 0xbeef0000U;
  require(shared.mach_namespaces.install(process.pid, notification_name,
                                         notification_object, send_type),
          "IOKit notification port namespace installation failed");
  require(memory.write32(message + 28, notification_name),
          "IOKit notification port write failed");
  require(memory.write32(message + 52,
                         static_cast<std::uint32_t>(notification_type.size())),
          "IOKit type count write failed");
  require(memory.copy_in(message + 56, type_bytes), "IOKit type write failed");
  std::array<std::uint32_t,
             device_mig::io_service_add_notification_arguments.size()>
      notification_counts{};
  notification_counts[1] = static_cast<std::uint32_t>(notification_type.size());
  const auto type_layout = xnu792::mig::compute_wire_layout(
      device_mig::io_service_add_notification_arguments,
      xnu792::mig::WireLayoutSide::Request, notification_counts);
  require(type_layout.has_value(),
          "IOKit notification type layout computation failed");
  const auto matching_count_offset = (*type_layout)[2].count_offset;
  require(memory.write32(message + matching_count_offset,
                         static_cast<std::uint32_t>(matching.size())),
          "IOKit matching count write failed");
  require(memory.copy_in(message + (*type_layout)[2].offset, matching),
          "IOKit matching write failed");
  notification_counts[2] = static_cast<std::uint32_t>(matching.size());
  const auto matching_layout = xnu792::mig::compute_wire_layout(
      device_mig::io_service_add_notification_arguments,
      xnu792::mig::WireLayoutSide::Request, notification_counts);
  require(matching_layout.has_value(),
          "IOKit notification matching layout computation failed");
  const auto reference_count_offset = (*matching_layout)[4].count_offset;
  require(memory.write32(message + reference_count_offset, 9),
          "IOKit oversized reference count write failed");
  require(handle_iokit_mach_request(
              memory, output, shared, process,
              device_mig::id(device_mig::Routine::io_service_add_notification),
              message, 48, 48, process.io_master_port, 0x10101) ==
              std::optional<std::uint32_t>{mach_receive_invalid_data},
          "IOKit notification accepted an oversized async reference");
  require(memory.write32(message + reference_count_offset, 0),
          "IOKit reference count reset failed");

  const auto result = handle_iokit_mach_request(
      memory, output, shared, process,
      device_mig::id(device_mig::Routine::io_service_add_notification), message,
      48, 48, process.io_master_port, 0x10101);
  require(result == std::optional<std::uint32_t>{0},
          "IOKit notification request failed");
  require(memory.read32(message + 20) == std::optional<std::uint32_t>{2949},
          "IOKit notification reply id mismatch");
  const auto iterator = memory.read32(message + 28).value_or(0);
  require(iterator >= 0x10000, "IOKit notification iterator missing");
  const auto iterator_object =
      shared.mach_namespaces.resolve(process.pid, iterator).value_or(0);
  require(iterator_object != 0 && iterator_object != iterator &&
              shared.iokit_iterators.contains(iterator_object),
          "IOKit iterator was not separated from its task-local name");
  require(shared.iokit_notifications.size() == 1,
          "IOKit notification registration was not retained");
  require(shared.iokit_notifications.front().type == "IOServiceFirstPublish",
          "IOKit notification type mismatch");
  require(shared.iokit_notifications.front().notification_port ==
              notification_object,
          "IOKit notification retained a task-local name as an object");

  const auto next_result =
      handle_iokit_mach_request(memory, output, shared, process, 2802, message,
                                48, 48, iterator, 0x10102);
  require(next_result == std::optional<std::uint32_t>{0},
          "IOKit iterator-next failed");
  require(memory.read32(message + 28) == std::optional<std::uint32_t>{0},
          "empty IOKit iterator did not terminate");

  constexpr std::string_view matching_xml{
      "<dict><key>IONameMatch</key><string>bluetooth</string></dict>\0", 63};
  std::array<std::byte, matching_xml.size()> matching_xml_bytes{};
  for (std::size_t index = 0; index < matching_xml.size(); ++index) {
    matching_xml_bytes[index] = static_cast<std::byte>(matching_xml[index]);
  }
  require(memory.write32(message + 36,
                         static_cast<std::uint32_t>(matching_xml.size())),
          "IOKit matching count write failed");
  require(memory.copy_in(message + 40, matching_xml_bytes),
          "IOKit matching dictionary write failed");
  const auto matching_result =
      handle_iokit_mach_request(memory, output, shared, process, 2804, message,
                                48, 48, process.io_master_port, 0x10103);
  require(matching_result == std::optional<std::uint32_t>{0},
          "IOKit matching-services request failed");
  const auto matching_iterator = memory.read32(message + 28).value_or(0);
  const auto matching_iterator_object =
      shared.mach_namespaces.resolve(process.pid, matching_iterator)
          .value_or(0);
  require(shared.iokit_iterators.contains(matching_iterator_object),
          "IOKit matching-services iterator was not retained");
  require((matching_iterator >> 8U) != (iterator >> 8U),
          "IOKit iterators reused one Mach port index");

  constexpr std::string_view display_matching{
      "<dict><key>IOProviderClass</key><string>AppleH1CLCD</string></dict>"};
  std::array<std::byte, display_matching.size()> display_matching_bytes{};
  for (std::size_t index = 0; index < display_matching.size(); ++index) {
    display_matching_bytes[index] =
        static_cast<std::byte>(display_matching[index]);
  }
  require(memory.write32(message + 36,
                         static_cast<std::uint32_t>(display_matching.size())),
          "display IOKit matching count write failed");
  require(memory.copy_in(message + 40, display_matching_bytes),
          "display IOKit matching dictionary write failed");
  require(handle_iokit_mach_request(memory, output, shared, process, 2804,
                                    message, 128, 128, process.io_master_port,
                                    0x10105) == std::optional<std::uint32_t>{0},
          "display IOKit matching-services request failed");
  const auto display_iterator = memory.read32(message + 28).value_or(0);
  require(handle_iokit_mach_request(memory, output, shared, process, 2802,
                                    message, 48, 48, display_iterator,
                                    0x10106) == std::optional<std::uint32_t>{0},
          "display IOKit iterator-next failed");
  const auto display_service = memory.read32(message + 28).value_or(0);
  const auto display_service_object =
      shared.mach_namespaces.resolve(process.pid, display_service).value_or(0);
  require(display_service != 0 && display_service_object != 0 &&
              shared.iokit_services.contains(display_service_object),
          "AppleH1CLCD display service was not registered");

  constexpr auto owning_task_offset =
      device_mig::io_service_open_arguments[1].request_offset;
  constexpr auto connect_type_offset =
      device_mig::io_service_open_arguments[2].request_offset;
  constexpr std::uint32_t standard_connect_type = 7;
  require(
      memory.write32(
          message + darwin::mig_wire::complex_descriptor_count_offset, 1) &&
          memory.write32(message + owning_task_offset, process.task_port) &&
          memory.write32(message + 40U, 0xfeedfaceU) &&
          memory.write32(message + connect_type_offset, standard_connect_type),
      "standard IOKit open request write failed");
  require(handle_iokit_mach_request(
              memory, output, shared, process,
              device_mig::id(device_mig::Routine::io_service_open), message, 48,
              48, display_service, 0x10107) == std::optional<std::uint32_t>{0},
          "standard IOKit open request failed");
  const auto standard_connection = memory.read32(message + 28).value_or(0);
  const auto standard_connection_object =
      shared.mach_namespaces.resolve(process.pid, standard_connection)
          .value_or(0);
  const auto standard_connection_state =
      shared.iokit_connections.find(standard_connection_object);
  require(standard_connection_state != shared.iokit_connections.end() &&
              standard_connection_state->second.type == standard_connect_type,
          "standard IOKit open used the wrong connect_type wire offset");
  require(handle_iokit_mach_request(
              memory, output, shared, process,
              static_cast<std::uint32_t>(iokit_abi::Message::ServiceClose),
              message, 44, 44, standard_connection,
              0x10108) == std::optional<std::uint32_t>{0},
          "standard IOKit connection close failed");

  require(memory.write32(
              message + darwin::mig_wire::complex_descriptor_count_offset,
              iokit_abi::service_open_extended::request_descriptor_count),
          "display open-extended descriptor count write failed");
  require(handle_iokit_mach_request(memory, output, shared, process, 2862,
                                    message, 60, 60, display_service,
                                    0x10107) == std::optional<std::uint32_t>{0},
          "display open-extended request failed");
  const auto display_connection = memory.read32(message + 28).value_or(0);
  const auto display_connection_object =
      shared.mach_namespaces.resolve(process.pid, display_connection)
          .value_or(0);
  require(display_connection != 0 &&
              display_connection_object != display_connection &&
              shared.iokit_connections.contains(display_connection_object) &&
              memory.read32(message +
                            iokit_abi::service_open_extended::result_offset) ==
                  std::optional<std::uint32_t>{0},
          "display open-extended did not return a live connection");

  using namespace iokit_abi::connect_method;
  constexpr std::uint32_t mach_request_bits = 0x00001513U;
  constexpr std::uint32_t method_reply_port = 0x10108U;
  constexpr std::uint32_t no_input_trailing_offset =
      scalar_input_offset + inband_count_size;
  const auto write_method_request = [&](std::uint32_t selector) {
    const std::array<std::uint32_t, minimum_request_size / 4U> request{
        mach_request_bits,
        minimum_request_size,
        display_connection,
        method_reply_port,
        0,
        static_cast<std::uint32_t>(iokit_abi::Message::ConnectMethod),
        0,
        1,
        selector,
        0, // scalar input count
        0, // inband input count
        0, // ool input address low
        0, // ool input address high
        1, // scalar output capacity
        0, // inband output capacity
        0, // ool output address
        0, // ool output size
    };
    for (std::size_t index = 0; index < request.size(); ++index) {
      require(memory.write32(message + static_cast<std::uint32_t>(index * 4U),
                             request[index]),
              "IOKit connect-method request write failed");
    }
    require(no_input_trailing_offset == 44U,
            "IOKit connect-method no-input layout drifted");
  };

  write_method_request(static_cast<std::uint32_t>(
      iokit_abi::AppleH1ClcdSelector::GetLayerDefaultSurface));
  require(handle_iokit_mach_request(
              memory, output, shared, process,
              static_cast<std::uint32_t>(iokit_abi::Message::ConnectMethod),
              message, minimum_request_size, firmware_receive_buffer_size,
              display_connection,
              method_reply_port) == std::optional<std::uint32_t>{0},
          "AppleH1CLCD default-surface method failed");
  constexpr auto default_surface_reply_size = reply_size(1, 0);
  require(memory.read32(message + 4) ==
                  std::optional<std::uint32_t>{default_surface_reply_size} &&
              memory.read32(message + 20) ==
                  std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(
                          iokit_abi::Message::ConnectMethod) +
                      100U},
          "IOKit connect-method reply header mismatch");
  require(memory.read32(message + return_code_offset) ==
                  std::optional<std::uint32_t>{iokit_abi::success} &&
              memory.read32(message + scalar_output_count_offset) ==
                  std::optional<std::uint32_t>{1} &&
              memory.read32(message + scalar_output_offset) ==
                  std::optional<std::uint32_t>{
                      iokit_abi::apple_h1clcd_default_surface_id} &&
              memory.read32(message + scalar_output_offset + 4U) ==
                  std::optional<std::uint32_t>{0} &&
              memory.read32(message + inband_output_count_offset(1)) ==
                  std::optional<std::uint32_t>{0} &&
              memory.read32(
                  message + out_of_line_output_size_offset(1, 0)) ==
                  std::optional<std::uint32_t>{0},
          "AppleH1CLCD default-surface scalar reply mismatch");
  validate_iokit_connect_method_reply_with_firmware(memory, message);

  constexpr std::uint32_t unsupported_display_selector = 0xffffU;
  write_method_request(unsupported_display_selector);
  require(handle_iokit_mach_request(
              memory, output, shared, process,
              static_cast<std::uint32_t>(iokit_abi::Message::ConnectMethod),
              message, minimum_request_size, firmware_receive_buffer_size,
              display_connection,
              method_reply_port) == std::optional<std::uint32_t>{0} &&
              memory.read32(message + 4) ==
                  std::optional<std::uint32_t>{minimum_reply_size} &&
              memory.read32(message + return_code_offset) ==
                  std::optional<std::uint32_t>{iokit_abi::unsupported} &&
              memory.read32(message + scalar_output_count_offset) ==
                  std::optional<std::uint32_t>{0},
          "unsupported AppleH1CLCD selector did not return IOReturn");

  write_method_request(static_cast<std::uint32_t>(
      iokit_abi::AppleH1ClcdSelector::GetLayerDefaultSurface));
  require(memory.write32(message + 4U, minimum_request_size - 1U) &&
              handle_iokit_mach_request(
                  memory, output, shared, process,
                  static_cast<std::uint32_t>(iokit_abi::Message::ConnectMethod),
                  message, minimum_request_size - 1U,
                  firmware_receive_buffer_size, display_connection,
                  method_reply_port) ==
                  std::optional<std::uint32_t>{mach_receive_invalid_data},
          "truncated io_connect_method request was accepted");

  require(handle_iokit_mach_request(memory, output, shared, process, 2816,
                                    message, 44, 44, display_connection,
                                    0x10109) == std::optional<std::uint32_t>{0},
          "display close request failed");
  require(!shared.iokit_connections.contains(display_connection_object) &&
              !shared.mach_namespaces.contains(process.pid, display_connection),
          "display connection survived io_service_close");
}

void shared_memory_syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t name_address = 0x38000;
  require(memory.map(name_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "shared-memory name map failed");
  constexpr std::string_view name{"/notify-test\0", 13};
  std::array<std::byte, name.size()> name_bytes{};
  for (std::size_t index = 0; index < name.size(); ++index) {
    name_bytes[index] = static_cast<std::byte>(name[index]);
  }
  require(memory.copy_in(name_address, name_bytes),
          "shared-memory name copy failed");

  const auto test_directory =
      std::filesystem::temp_directory_path() / "ilemu-tests";
  std::error_code filesystem_error;
  std::filesystem::remove_all(test_directory, filesystem_error);
  std::filesystem::create_directories(test_directory / "rootfs",
                                      filesystem_error);
  require(!filesystem_error, "shared-memory test root creation failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output, test_directory / "rootfs"};

  cpu.registers()[0] = name_address;
  cpu.registers()[1] = 0x0202; // O_CREAT | O_RDWR
  cpu.registers()[2] = 0644;
  cpu.registers()[12] = 266;
  kernel.dispatch(cpu, 0x80);
  const auto fd = cpu.registers()[0];
  require(fd >= 3, "shm_open did not return a descriptor");

  cpu.registers()[0] = fd;
  cpu.registers()[1] = AddressSpace::page_size;
  cpu.registers()[2] = 0;
  cpu.registers()[12] = 201;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "ftruncate on shared memory failed");

  cpu.registers()[0] = 0;
  cpu.registers()[1] = AddressSpace::page_size;
  cpu.registers()[2] = 3; // PROT_READ | PROT_WRITE
  cpu.registers()[3] = 1; // MAP_SHARED
  cpu.registers()[4] = fd;
  cpu.registers()[5] = 0;
  cpu.registers()[6] = 0;
  cpu.registers()[12] = 197;
  kernel.dispatch(cpu, 0x80);
  require(memory.mapped(cpu.registers()[0]),
          "shared-memory mmap did not map its result");

  cpu.registers()[0] = name_address;
  cpu.registers()[12] = 267;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "shm_unlink failed");
  std::filesystem::remove_all(test_directory, filesystem_error);
}

void arm_fast_trap_test() {
  AddressSpace memory;
  constexpr std::uint32_t code_address = 0x48000;
  require(memory.map(code_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "ARM fast-trap code page map failed");
  require(memory.write32(code_address, 0xe12fff1eU),
          "ARM fast-trap code write failed");
  require(!memory.read32(code_address, MemoryPermission::Execute),
          "writable test page was executable before cache invalidation");

  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu cpu0{0, memory, monitor};
  Cpu cpu1{1, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu0.registers()[12] = darwin::arm_fast_trap::syscall_number;
  cpu0.registers()[3] = darwin::arm_fast_trap::thread_set_cthread_self;
  cpu0.registers()[0] = 0x12345678U;
  kernel.dispatch(cpu0, 0x80);
  require(cpu0.registers()[0] == 0x12345678U,
          "cthread set trap did not preserve r0");

  cpu0.registers()[3] = darwin::arm_fast_trap::thread_get_cthread_self;
  cpu0.registers()[0] = 0;
  kernel.dispatch(cpu0, 0x80);
  require(cpu0.registers()[0] == 0x12345678U,
          "cthread get trap returned the wrong thread-local value");

  cpu1.registers()[12] = darwin::arm_fast_trap::syscall_number;
  cpu1.registers()[3] = darwin::arm_fast_trap::thread_get_cthread_self;
  cpu1.registers()[0] = 0xffffffffU;
  kernel.dispatch(cpu1, 0x80);
  require(cpu1.registers()[0] == 0,
          "cthread self leaked between emulated threads");

  cpu0.registers()[3] = darwin::arm_fast_trap::instruction_cache_invalidate;
  cpu0.registers()[0] = code_address;
  cpu0.registers()[1] = 20;
  kernel.dispatch(cpu0, 0x80);
  require(cpu0.registers()[0] == code_address && cpu0.registers()[1] == 20,
          "instruction-cache trap changed saved registers");
  require(memory.read32(code_address, MemoryPermission::Execute) ==
              std::optional<std::uint32_t>{0xe12fff1eU},
          "instruction-cache trap did not enable generated ARM code");

  cpu0.registers()[3] = darwin::arm_fast_trap::data_cache_flush;
  kernel.dispatch(cpu0, 0x80);
  require(cpu0.registers()[0] == code_address,
          "data-cache trap changed saved registers");
}

void memory_protect_syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t page = 0x4a000;
  require(memory.map(page, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "mprotect test page map failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = page;
  cpu.registers()[1] = AddressSpace::page_size;
  cpu.registers()[2] = 5; // VM_PROT_READ | VM_PROT_EXECUTE
  cpu.registers()[12] = darwin::syscall::memory_protect;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && (cpu.cpsr() & (1U << 29U)) == 0,
          "mprotect did not report success");
  require(memory.read32(page, MemoryPermission::Execute).has_value(),
          "mprotect did not add execute permission");
  require(!memory.write32(page, 0), "mprotect did not remove write permission");

  cpu.registers()[0] = page + 1;
  cpu.registers()[1] = AddressSpace::page_size;
  cpu.registers()[2] = 1;
  cpu.registers()[12] = darwin::syscall::memory_protect;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::error::invalid_argument &&
              (cpu.cpsr() & (1U << 29U)) != 0,
          "mprotect accepted an unaligned address");
}

void kill_syscall_test() {
  AddressSpace memory;
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = kernel.process().pid;
  cpu.registers()[1] = 0;
  cpu.registers()[12] = darwin::syscall::kill;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && !kernel.process().exited,
          "kill(pid, 0) changed process state");

  cpu.registers()[0] = kernel.process().pid;
  cpu.registers()[1] = darwin::signal::count;
  cpu.registers()[12] = darwin::syscall::kill;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == darwin::error::invalid_argument,
          "kill accepted an invalid signal number");

  cpu.registers()[0] = kernel.process().pid;
  cpu.registers()[1] = darwin::signal::abort;
  cpu.registers()[12] = darwin::syscall::kill;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && kernel.process().exited &&
              kernel.process().termination_signal == darwin::signal::abort,
          "default SIGABRT did not terminate the guest process");
}

void credential_syscall_test() {
  AddressSpace memory;
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  cpu.registers()[0] = 20;
  cpu.registers()[12] = darwin::syscall::set_effective_group_id;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && kernel.process().gid == 0 &&
              kernel.process().effective_gid == 20,
          "setegid changed the real group or missed the effective group");

  cpu.registers()[12] = 43; // getegid
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 20, "getegid missed the credential update");
  cpu.registers()[12] = 47; // getgid
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "getgid returned the effective group");

  cpu.registers()[0] = 501;
  cpu.registers()[12] = darwin::syscall::set_effective_user_id;
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0 && kernel.process().uid == 0 &&
              kernel.process().effective_uid == 501,
          "seteuid changed the real user or missed the effective user");
  cpu.registers()[12] = 25; // geteuid
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 501, "geteuid missed the credential update");
  cpu.registers()[12] = 24; // getuid
  kernel.dispatch(cpu, 0x80);
  require(cpu.registers()[0] == 0, "getuid returned the effective user");
}

void concurrent_wait_syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t status_page = 0x52000;
  require(memory.map(status_page, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "wait status page map failed");
  Dynarmic::ExclusiveMonitor monitor{2};
  Cpu first{0, memory, monitor};
  Cpu second{1, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  first.registers()[0] = 40;
  first.registers()[1] = status_page;
  first.registers()[12] = 7; // wait4
  kernel.dispatch(first, 0x80);
  second.registers()[0] = 41;
  second.registers()[1] = status_page + sizeof(std::uint32_t);
  second.registers()[12] = 7;
  kernel.dispatch(second, 0x80);
  require(kernel.pending_waits().size() == 2,
          "one thread's wait4 replaced another thread's wait");

  require(kernel.complete_wait(first, 40, 0x1200) &&
              kernel.pending_waits().size() == 1 &&
              memory.read32(status_page) == 0x1200,
          "completing wait4 removed the wrong pending thread");
  require(kernel.complete_wait(second, 41, 9) &&
              kernel.pending_waits().empty() &&
              memory.read32(status_page + sizeof(std::uint32_t)) == 9,
          "second concurrent wait4 did not complete independently");
}

void wall_clock_syscall_test() {
  AddressSpace memory;
  constexpr std::uint32_t timeval_address = 0x53000;
  require(memory.map(timeval_address, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "gettimeofday result page map failed");
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  CompatibilityKernel kernel{memory, output};

  constexpr std::uint64_t synchronized_seconds = 1'800'000'000ULL;
  constexpr std::uint64_t synchronized_nanoseconds = 123'456'000ULL;
  kernel.synchronize_wall_time(
      synchronized_seconds * VirtualClock::nanoseconds_per_second +
      synchronized_nanoseconds);

  const auto get_time = [&] {
    // Match libSystem's ___gettimeofday stub: r3 retains the destination
    // while the kernel returns timeval.tv_sec/tv_usec in r0/r1.
    cpu.registers()[0] = timeval_address;
    cpu.registers()[1] = 0;
    cpu.registers()[3] = timeval_address;
    cpu.registers()[12] = 116; // gettimeofday
    kernel.dispatch(cpu, 0x80);
    const auto success = (cpu.cpsr() & (1U << 29U)) == 0;
    if (success) {
      static_cast<void>(memory.write32(cpu.registers()[3], cpu.registers()[0]));
      static_cast<void>(
          memory.write32(cpu.registers()[3] + sizeof(std::uint32_t),
                         cpu.registers()[1]));
    }
    return std::pair{memory.read32(timeval_address),
                     memory.read32(timeval_address + sizeof(std::uint32_t))};
  };

  const auto initial = get_time();
  require(initial.first == synchronized_seconds &&
              initial.second == synchronized_nanoseconds / 1'000ULL,
          "gettimeofday did not use the synchronized wall clock");

  kernel.advance_time_by(2 * VirtualClock::nanoseconds_per_second + 500'000);
  const auto advanced = get_time();
  require(advanced.first == synchronized_seconds + 2 &&
              advanced.second == 123'956,
          "synchronized gettimeofday did not advance with monotonic time");
}

void run_tests() {
  address_space_test();
  display_state_test();
  cpu_cluster_test();
  cpu_deferred_svc_test();
  cpu_debug_breakpoint_test();
  userland_hle_dispatch_test();
  layerkit_root_compatibility_test();
  gdb_rsp_protocol_test();
  syscall_test();
  resource_limit_syscall_test();
  iokit_notification_test();
  shared_memory_syscall_test();
  arm_fast_trap_test();
  memory_protect_syscall_test();
  kill_syscall_test();
  credential_syscall_test();
  concurrent_wait_syscall_test();
  wall_clock_syscall_test();
  ilemu::test::kernel::run_sysctl_tests();
  ilemu::test::kernel::run_device_tests();
  ilemu::test::kernel::run_iokit_power_tests();
  ilemu::test::kernel::run_iokit_display_tests();
}

} // namespace

int main() { return ilemu::test::run_suite("kernel", run_tests); }
