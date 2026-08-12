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

#include "suite.hpp"

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

#include "test_support.hpp"

namespace {

using namespace ilemu;
using ilemu::test::require;

void core_surface_firmware_hle_test() {
  const std::array candidates{
      std::filesystem::path{
          "build/rootfs/System/Library/Frameworks/CoreSurface.framework/"
          "CoreSurface"},
      std::filesystem::path{
          "rootfs/System/Library/Frameworks/CoreSurface.framework/"
          "CoreSurface"},
  };
  const auto path = std::find_if(
      candidates.begin(), candidates.end(),
      [](const auto &candidate) { return std::filesystem::exists(candidate); });
  if (path == candidates.end())
    return;

  AddressSpace memory;
  const auto image = MachOImage::parse(*path);
  image.map_into(memory);
  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  UserlandHleRegistry registry{memory, output};
  auto display = std::make_shared<DisplayState>();
  auto shared_state = std::make_shared<KernelSharedState>();
  CoreSurfaceHle core_surface{registry, display};
  core_surface.set_shared_state(shared_state);
  cpu.set_svc_handler([&](Cpu &source, std::uint32_t immediate) {
    require(registry.dispatch(source, 1, immediate),
            "CoreSurface firmware issued an unregistered HLE SVC");
  });

  std::size_t installed = 0;
  for (const auto &segment : image.segments()) {
    if (segment.file_size == 0)
      continue;
    installed +=
        registry.install_mapped_image(cpu, 1, *path, segment.vm_address,
                                      segment.file_size, segment.file_offset);
  }
  require(installed >= 20,
          "CoreSurface client-buffer firmware exports were not intercepted");

  const auto invoke = [&](std::string_view symbol,
                          std::array<std::uint32_t, 3> arguments) {
    const auto *entry = image.find_symbol(symbol);
    require(entry != nullptr, "CoreSurface firmware symbol is missing");
    cpu.clear_halt();
    cpu.registers()[0] = arguments[0];
    cpu.registers()[1] = arguments[1];
    cpu.registers()[2] = arguments[2];
    cpu.registers()[14] = 0x1000;
    cpu.registers()[15] = entry->value;
    cpu.set_cpsr(0x10);
    const auto result = cpu.step();
    require(result.exception.empty() && !result.fault,
            "CoreSurface firmware HLE entry faulted");
    return cpu.registers()[0];
  };

  const auto client = invoke("_CoreSurfaceClientBufferCreate", {0, 0, 0});
  require(client != 0, "CoreSurface client buffer creation failed");
  require(
      invoke("_CoreSurfaceClientBufferGetWidth", {client, 0, 0}) ==
              default_display_width &&
          invoke("_CoreSurfaceClientBufferGetHeight", {client, 0, 0}) ==
              default_display_height &&
          invoke("_CoreSurfaceClientBufferGetBytesPerRow", {client, 0, 0}) ==
              default_display_width * 4U,
      "CoreSurface client buffer geometry does not match the device profile");
  const auto base =
      invoke("_CoreSurfaceClientBufferGetBaseAddress", {client, 0, 0});
  require(base != 0 && memory.write32(base, 0xff112233U),
          "CoreSurface client pixel memory is unavailable");
  require(invoke("_CoreSurfaceClientBufferUnlock", {client, 0, 0}) == 0,
          "CoreSurface client unlock failed");
  require(display->snapshot().pixels.front() == 0xff112233U,
          "CoreSurface client pixels were not submitted to the display");
}

void mbx2d_surface_composition_test() {
  const std::array core_candidates{
      std::filesystem::path{
          "build/rootfs/System/Library/Frameworks/CoreSurface.framework/"
          "CoreSurface"},
      std::filesystem::path{
          "rootfs/System/Library/Frameworks/CoreSurface.framework/"
          "CoreSurface"},
  };
  const std::array mbx_candidates{
      std::filesystem::path{
          "build/rootfs/System/Library/Frameworks/MBX2D.framework/MBX2D"},
      std::filesystem::path{
          "rootfs/System/Library/Frameworks/MBX2D.framework/MBX2D"},
  };
  const auto core_path = std::find_if(
      core_candidates.begin(), core_candidates.end(),
      [](const auto &candidate) { return std::filesystem::exists(candidate); });
  const auto mbx_path = std::find_if(
      mbx_candidates.begin(), mbx_candidates.end(),
      [](const auto &candidate) { return std::filesystem::exists(candidate); });
  if (core_path == core_candidates.end() || mbx_path == mbx_candidates.end()) {
    return;
  }

  AddressSpace memory;
  const auto core_image = MachOImage::parse(*core_path);
  const auto mbx_image = MachOImage::parse(*mbx_path);
  core_image.map_into(memory);
  mbx_image.map_into(memory);
  constexpr std::uint32_t stack = 0x90000U;
  require(memory.map(stack, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "MBX2D HLE stack map failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  UserlandHleRegistry registry{memory, output};
  auto display = std::make_shared<DisplayState>();
  auto surfaces = std::make_shared<SurfaceStore>();
  CoreSurfaceHle core_surface{registry, display, surfaces};
  Mbx2dHle mbx2d{registry, display, surfaces};
  cpu.set_svc_handler([&](Cpu &source, std::uint32_t immediate) {
    require(registry.dispatch(source, 31, immediate),
            "graphics firmware issued an unregistered HLE SVC");
  });

  const auto install = [&](const MachOImage &image,
                           const std::filesystem::path &path) {
    std::size_t count = 0;
    for (const auto &segment : image.segments()) {
      if (segment.file_size == 0)
        continue;
      count +=
          registry.install_mapped_image(cpu, 31, path, segment.vm_address,
                                        segment.file_size, segment.file_offset);
    }
    return count;
  };
  require(install(core_image, *core_path) >= 20,
          "CoreSurface composition hooks were not installed");
  require(install(mbx_image, *mbx_path) >= 20,
          "MBX2D composition hooks were not installed");

  const auto invoke = [&](const MachOImage &image, std::string_view symbol,
                          const std::vector<std::uint32_t> &arguments) {
    const auto *entry = image.find_symbol(symbol);
    require(entry != nullptr, "graphics firmware symbol is missing");
    cpu.clear_halt();
    for (std::size_t index = 0; index < 4; ++index) {
      cpu.registers()[index] = index < arguments.size() ? arguments[index] : 0;
    }
    for (std::size_t index = 4; index < arguments.size(); ++index) {
      require(
          memory.write32(stack + static_cast<std::uint32_t>((index - 4U) * 4U),
                         arguments[index]),
          "graphics HLE stack argument write failed");
    }
    cpu.registers()[13] = stack;
    cpu.registers()[14] = 0x1000U;
    cpu.registers()[15] = entry->value;
    cpu.set_cpsr(0x10);
    const auto result = cpu.step();
    require(result.exception.empty() && !result.fault,
            "graphics firmware HLE entry faulted");
    return cpu.registers()[0];
  };
  const auto core_call = [&](std::string_view symbol,
                             std::vector<std::uint32_t> arguments) {
    return invoke(core_image, symbol, arguments);
  };
  const auto mbx_call = [&](std::string_view symbol,
                            std::vector<std::uint32_t> arguments) {
    return invoke(mbx_image, symbol, arguments);
  };
  require(mbx_call("_mbx2DInitialize", {}) == mbx2d_abi::success &&
              mbx_call("_mbx2DInitialize", {}) == mbx2d_abi::success,
          "MBX2D initialization is not idempotent");

  const auto source_client = core_call("_CoreSurfaceClientBufferCreate", {});
  const auto source_id =
      core_call("_CoreSurfaceClientBufferGetID", {source_client});
  const auto source_base =
      core_call("_CoreSurfaceClientBufferGetBaseAddress", {source_client});
  const auto source_pitch =
      core_call("_CoreSurfaceClientBufferGetBytesPerRow", {source_client});
  const auto source_surface = mbx_call("_mbx2DCreateSurface", {source_id});
  require(source_client != 0 && source_id != 0 && source_base != 0 &&
              source_surface != 0,
          "source CoreSurface/MBX2D binding failed");
  require(mbx_call("_mbx2DSetDestinationSurface",
                   {source_surface, source_pitch, mbx2d_abi::pixel_format_bgra,
                    0}) == mbx2d_abi::success,
          "MBX2D source destination binding failed");
  constexpr std::uint32_t first_color = 0xff224466U;
  require(mbx_call("_mbx2DBlitColor", {10, 20, 2, 2, first_color}) ==
                  mbx2d_abi::success &&
              memory.read32(source_base + 20U * source_pitch + 10U * 4U) ==
                  std::optional<std::uint32_t>{first_color},
          "MBX2D color fill did not update CoreSurface guest memory");

  const auto destination_client =
      core_call("_CoreSurfaceClientBufferCreate", {});
  const auto destination_id =
      core_call("_CoreSurfaceClientBufferGetID", {destination_client});
  const auto destination_base =
      core_call("_CoreSurfaceClientBufferGetBaseAddress", {destination_client});
  const auto destination_pitch =
      core_call("_CoreSurfaceClientBufferGetBytesPerRow", {destination_client});
  const auto destination_surface =
      mbx_call("_mbx2DCreateSurface", {destination_id});
  require(mbx_call("_mbx2DSetSourceSurface",
                   {source_surface, source_pitch, mbx2d_abi::pixel_format_bgra,
                    0}) == mbx2d_abi::success &&
              mbx_call("_mbx2DSetDestinationSurface",
                       {destination_surface, destination_pitch,
                        mbx2d_abi::pixel_format_bgra, 0}) == mbx2d_abi::success,
          "MBX2D source/destination setup failed");
  require(mbx_call("_mbx2DBlitCopy", {10, 20, 30, 40, 2, 2}) ==
                  mbx2d_abi::success &&
              memory.read32(destination_base + 40U * destination_pitch +
                            30U * 4U) ==
                  std::optional<std::uint32_t>{first_color},
          "MBX2D copy did not transfer CoreSurface pixels");

  require(mbx_call("_mbx2DSetScissor", {31, 41, 32, 42}) == mbx2d_abi::success,
          "MBX2D scissor setup failed");
  constexpr std::uint32_t clipped_color = 0xffaa5500U;
  require(mbx_call("_mbx2DBlitColor", {30, 40, 3, 3, clipped_color}) ==
                  mbx2d_abi::success &&
              memory.read32(destination_base + 41U * destination_pitch +
                            31U * 4U) ==
                  std::optional<std::uint32_t>{clipped_color} &&
              memory.read32(destination_base + 40U * destination_pitch +
                            30U * 4U) ==
                  std::optional<std::uint32_t>{first_color},
          "MBX2D scissor did not clip the color fill");

  constexpr std::uint32_t post_affine_color = 0xff3070b0U;
  require(
      mbx_call("_mbx2DEnable", {mbx2d_abi::feature_rotation}) ==
              mbx2d_abi::success &&
          mbx_call("_mbx2DDisable", {mbx2d_abi::feature_rotation}) ==
              mbx2d_abi::success &&
          mbx_call("_mbx2DBlitColor", {100, 100, 1, 1, post_affine_color}) ==
              mbx2d_abi::success &&
          memory.read32(destination_base + 100U * destination_pitch +
                        100U * 4U) ==
              std::optional<std::uint32_t>{post_affine_color},
      "MBX2D affine scissor leaked into later LayerKit blits");

  require(mbx_call("_mbx2DSetScissor",
                   {0, 0, default_display_width, default_display_height}) ==
              mbx2d_abi::success,
          "MBX2D full-screen scissor setup failed");
  constexpr std::uint32_t premultiplied_half_red = 0x80800000U;
  constexpr std::uint32_t opaque_half_blue = 0xff000080U;
  constexpr std::uint32_t source_over_result = 0xff800040U;
  require(
      memory.write32(source_base + 70U * source_pitch + 60U * 4U,
                     premultiplied_half_red) &&
          memory.write32(destination_base + 80U * destination_pitch + 70U * 4U,
                         opaque_half_blue),
      "MBX2D blend test pixels could not be initialized");
  require(mbx_call("_mbx2DSetBlendEquation",
                   {mbx2d_abi::layerkit_source_over_source_word,
                    mbx2d_abi::layerkit_source_over_destination_word, 0xffU}) ==
                  mbx2d_abi::success &&
              mbx_call("_mbx2DEnable", {mbx2d_abi::feature_blend}) ==
                  mbx2d_abi::success &&
              mbx_call("_mbx2DBlitCopy", {60, 70, 70, 80, 1, 1}) ==
                  mbx2d_abi::success &&
              memory.read32(destination_base + 80U * destination_pitch +
                            70U * 4U) ==
                  std::optional<std::uint32_t>{source_over_result},
          "MBX2D premultiplied source-over blend is incorrect");
  require(
      memory.write32(destination_base + 80U * destination_pitch + 71U * 4U,
                     opaque_half_blue) &&
          mbx_call("_mbx2DBlitColor", {71, 80, 1, 1, premultiplied_half_red}) ==
              mbx2d_abi::success &&
          memory.read32(destination_base + 80U * destination_pitch +
                        71U * 4U) ==
              std::optional<std::uint32_t>{source_over_result},
      "MBX2D blended color fill is incorrect");
  constexpr std::uint32_t opaque_red = 0xffff0000U;
  constexpr std::uint32_t opaque_blue = 0xff0000ffU;
  constexpr std::uint32_t quarter_red_three_quarters_blue = 0xff4000bfU;
  require(
      memory.write32(source_base + 70U * source_pitch + 61U * 4U, opaque_red) &&
          memory.write32(destination_base + 80U * destination_pitch + 72U * 4U,
                         opaque_blue) &&
          mbx_call("_mbx2DSetBlendEquation",
                   {mbx2d_abi::layerkit_crossfade_source_word,
                    mbx2d_abi::layerkit_crossfade_destination_word, 64U}) ==
              mbx2d_abi::success &&
          mbx_call("_mbx2DBlitCopy", {61, 70, 72, 80, 1, 1}) ==
              mbx2d_abi::success &&
          memory.read32(destination_base + 80U * destination_pitch +
                        72U * 4U) ==
              std::optional<std::uint32_t>{quarter_red_three_quarters_blue},
      "MBX2D constant-alpha crossfade is incorrect");
  require(mbx_call("_mbx2DSetBlendEquationComplex",
                   {mbx2d_abi::layerkit_mask_source_word,
                    mbx2d_abi::layerkit_mask_destination_word,
                    mbx2d_abi::layerkit_mask_operation_word, 0xffU}) ==
              mbx2d_abi::success,
          "MBX2D straight-alpha equation was rejected");
  constexpr std::uint32_t straight_half_red = 0x80ff0000U;
  constexpr std::uint32_t straight_source_over_result = 0xff80007fU;
  require(
      memory.write32(source_base + 70U * source_pitch + 62U * 4U,
                     straight_half_red) &&
          memory.write32(destination_base + 80U * destination_pitch + 73U * 4U,
                         opaque_blue) &&
          mbx_call("_mbx2DBlitCopy", {62, 70, 73, 80, 1, 1}) ==
              mbx2d_abi::success &&
          memory.read32(destination_base + 80U * destination_pitch +
                        73U * 4U) ==
              std::optional<std::uint32_t>{straight_source_over_result},
      "MBX2D straight-alpha source-over blend is incorrect");
  require(mbx_call("_mbx2DSetBlendEquationComplex",
                   {0, mbx2d_abi::layerkit_mask_destination_word,
                    mbx2d_abi::layerkit_mask_operation_word, 0xffU}) ==
              mbx2d_abi::failure,
          "MBX2D complex blend validation differs from firmware");
  require(mbx_call("_mbx2DDisable", {mbx2d_abi::feature_blend}) ==
              mbx2d_abi::success,
          "MBX2D blend disable failed");

  constexpr std::array<std::uint32_t, 2> scale_source{0xff010203U, 0xffa0b0c0U};
  require(memory.write32(source_base, scale_source[0]) &&
              memory.write32(source_base + 4U, scale_source[1]),
          "MBX2D scale source could not be initialized");
  require(mbx_call("_mbx2DSetScaleFactor", {std::bit_cast<std::uint32_t>(2.0F),
                                            mbx2d_abi::float_one_bits}) ==
                  mbx2d_abi::success &&
              mbx_call("_mbx2DBlitCopy", {0, 0, 100, 100, 2, 1}) ==
                  mbx2d_abi::success,
          "MBX2D scaled copy failed");
  const auto scaled_row =
      destination_base + 100U * destination_pitch + 100U * 4U;
  require(memory.read32(scaled_row) ==
                  std::optional<std::uint32_t>{scale_source[0]} &&
              memory.read32(scaled_row + 4U) ==
                  std::optional<std::uint32_t>{scale_source[0]} &&
              memory.read32(scaled_row + 8U) ==
                  std::optional<std::uint32_t>{scale_source[1]} &&
              memory.read32(scaled_row + 12U) ==
                  std::optional<std::uint32_t>{scale_source[1]},
          "MBX2D nearest-neighbor scale pixels are incorrect");

  constexpr std::array<std::uint32_t, 6> rotation_source{
      0xff000001U, 0xff000002U, 0xff000003U,
      0xff000004U, 0xff000005U, 0xff000006U};
  for (std::size_t index = 0; index < rotation_source.size(); ++index) {
    const auto x = static_cast<std::uint32_t>(index % 2U);
    const auto y = static_cast<std::uint32_t>(index / 2U);
    require(memory.write32(source_base + y * source_pitch + x * 4U,
                           rotation_source[index]),
            "MBX2D rotation source could not be initialized");
  }
  require(
      mbx_call("_mbx2DSetScaleFactor",
               {mbx2d_abi::float_one_bits, mbx2d_abi::float_one_bits}) ==
              mbx2d_abi::success &&
          mbx_call("_mbx2DSetRotation", {mbx2d_abi::rotation_clockwise_90}) ==
              mbx2d_abi::success &&
          mbx_call("_mbx2DEnable", {mbx2d_abi::feature_rotation}) ==
              mbx2d_abi::success &&
          mbx_call("_mbx2DBlitCopy", {0, 0, 120, 120, 2, 3}) ==
              mbx2d_abi::success,
      "MBX2D clockwise rotation failed");
  constexpr std::array<std::uint32_t, 6> rotation_expected{
      rotation_source[4], rotation_source[2], rotation_source[0],
      rotation_source[5], rotation_source[3], rotation_source[1]};
  for (std::size_t index = 0; index < rotation_expected.size(); ++index) {
    const auto x = static_cast<std::uint32_t>(index % 3U);
    const auto y = static_cast<std::uint32_t>(index / 3U);
    require(memory.read32(destination_base + (120U + y) * destination_pitch +
                          (120U + x) * 4U) ==
                std::optional<std::uint32_t>{rotation_expected[index]},
            "MBX2D clockwise rotation pixels are incorrect");
  }
  require(mbx_call("_mbx2DDisable", {mbx2d_abi::feature_rotation}) ==
                  mbx2d_abi::success &&
              mbx_call("_mbx2DEnable", {mbx2d_abi::feature_rotation + 1U}) ==
                  mbx2d_abi::failure,
          "MBX2D feature-word validation differs from firmware");

  const auto context = mbx_call("_mbx2DCtxInitialize", {});
  require(
      context != 0 &&
          mbx_call("_mbx2DCtxSetSourceSurface",
                   {context, source_surface, source_pitch,
                    mbx2d_abi::pixel_format_bgra, 0}) == mbx2d_abi::success &&
          mbx_call("_mbx2DCtxSetDestinationSurface",
                   {context, destination_surface, destination_pitch,
                    mbx2d_abi::pixel_format_bgra, 0}) == mbx2d_abi::success &&
          mbx_call("_mbx2DCtxBlitCopy", {context, 0, 0, 140, 140, 1, 1}) ==
              mbx2d_abi::success &&
          memory.read32(destination_base + 140U * destination_pitch +
                        140U * 4U) ==
              std::optional<std::uint32_t>{rotation_source[0]} &&
          mbx_call("_mbx2DCtxTerminate", {context}) == mbx2d_abi::success,
      "MBX2D context-local composition state failed");
  constexpr std::uint32_t flush_surface_array = stack + 0x200U;
  require(memory.write32(flush_surface_array, destination_surface) &&
              mbx_call("_mbx2DFlushSurfaces", {flush_surface_array, 1}) ==
                  mbx2d_abi::success &&
              mbx_call("_mbx2DFlushInvalidateSurfaces",
                       {flush_surface_array, 1}) == mbx2d_abi::success,
          "MBX2D surface cache synchronization failed");

  constexpr std::uint32_t client_pixels = stack + 0x300U;
  constexpr std::uint32_t client_handle_output = stack + 0x320U;
  constexpr std::uint32_t quad_positions = stack + 0x340U;
  constexpr std::uint32_t quad_texture = stack + 0x360U;
  constexpr std::array<std::uint32_t, 2> stripe{0xff123456U, 0xffabcdefU};
  require(memory.write32(client_pixels, stripe[0]) &&
              memory.write32(client_pixels + 4U, stripe[1]) &&
              mbx_call("_mbx2DAddClientSurface",
                       {client_pixels, 8, client_handle_output, 1}) ==
                  mbx2d_abi::success,
          "MBX2D raw client surface wrapping failed");
  const auto client_surface = memory.read32(client_handle_output).value_or(0);
  const std::array<float, 8> positions{200.0F, 200.0F, 204.0F, 200.0F,
                                       204.0F, 202.0F, 200.0F, 202.0F};
  const std::array<float, 8> texture{0.5F, 0.0F, 0.5F, 0.0F,
                                     0.5F, 2.0F, 0.5F, 2.0F};
  for (std::size_t index = 0; index < positions.size(); ++index) {
    require(
        memory.write32(quad_positions + static_cast<std::uint32_t>(index * 4U),
                       std::bit_cast<std::uint32_t>(positions[index])) &&
            memory.write32(quad_texture +
                               static_cast<std::uint32_t>(index * 4U),
                           std::bit_cast<std::uint32_t>(texture[index])),
        "MBX3D quad coordinates could not be initialized");
  }
  require(client_surface != 0 &&
              mbx_call("_mbx2DSetSourceSurface",
                       {client_surface, 4, mbx2d_abi::pixel_format_bgra, 0}) ==
                  mbx2d_abi::success &&
              mbx_call("_mbx3DQuadCopy", {quad_positions, quad_texture, 1, 2,
                                          1}) == mbx2d_abi::success &&
              memory.read32(destination_base + 200U * destination_pitch +
                            200U * 4U) == stripe[0] &&
              memory.read32(destination_base + 201U * destination_pitch +
                            203U * 4U) == stripe[1],
          "MBX3D QuadCopy did not scale a raw client surface");
  const std::array<float, 8> trapezoid_positions{
      220.0F, 200.0F, 224.0F, 201.0F, 224.0F, 205.0F, 220.0F, 206.0F};
  const std::array<float, 8> trapezoid_texture{0.5F, 0.0F, 0.5F, 0.0F,
                                               0.5F, 2.0F, 0.5F, 2.0F};
  for (std::size_t index = 0; index < trapezoid_positions.size(); ++index) {
    require(memory.write32(
                quad_positions + static_cast<std::uint32_t>(index * 4U),
                std::bit_cast<std::uint32_t>(trapezoid_positions[index])) &&
                memory.write32(
                    quad_texture + static_cast<std::uint32_t>(index * 4U),
                    std::bit_cast<std::uint32_t>(trapezoid_texture[index])),
            "MBX3D trapezoid coordinates could not be initialized");
  }
  require(mbx_call("_mbx3DQuadCopy", {quad_positions, quad_texture, 1, 2, 1}) ==
                  mbx2d_abi::success &&
              memory.read32(destination_base + 202U * destination_pitch +
                            221U * 4U) == stripe[0] &&
              memory.read32(destination_base + 204U * destination_pitch +
                            223U * 4U) == stripe[1],
          "MBX3D QuadCopy did not rasterize a non-axis-aligned quad");
  require(mbx_call("_mbx2DFinish", {destination_surface}) ==
                  mbx2d_abi::success &&
              display->snapshot().pixels[200U * default_display_width + 200U] ==
                  stripe[0],
          "MBX2D finish did not submit the destination surface");
  require(mbx_call("_mbx2DTerminate", {}) == mbx2d_abi::success &&
              mbx_call("_mbx2DTerminate", {}) == mbx2d_abi::failure,
          "MBX2D termination lifecycle differs from firmware");
}

void opengles_resource_firmware_boundary_test() {
  const std::array candidates{
      std::filesystem::path{
          "build/rootfs/System/Library/Frameworks/OpenGLES.framework/"
          "OpenGLES"},
      std::filesystem::path{
          "rootfs/System/Library/Frameworks/OpenGLES.framework/OpenGLES"},
  };
  const auto path = std::find_if(
      candidates.begin(), candidates.end(),
      [](const auto &candidate) { return std::filesystem::exists(candidate); });
  if (path == candidates.end())
    return;
  const std::array core_candidates{
      std::filesystem::path{
          "build/rootfs/System/Library/Frameworks/CoreSurface.framework/"
          "CoreSurface"},
      std::filesystem::path{
          "rootfs/System/Library/Frameworks/CoreSurface.framework/"
          "CoreSurface"},
  };
  const auto core_path = std::find_if(
      core_candidates.begin(), core_candidates.end(),
      [](const auto &candidate) { return std::filesystem::exists(candidate); });
  if (core_path == core_candidates.end())
    return;

  AddressSpace memory;
  const auto image = MachOImage::parse(*path);
  const auto core_image = MachOImage::parse(*core_path);
  image.map_into(memory);
  core_image.map_into(memory);
  constexpr std::uint32_t stack = 0xa0000U;
  constexpr std::uint32_t output_address = stack + 0x300U;
  constexpr std::uint32_t data_address = stack + 0x400U;
  require(memory.map(stack, AddressSpace::page_size,
                     MemoryPermission::Read | MemoryPermission::Write),
          "OpenGLES HLE stack map failed");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  UserlandHleRegistry registry{memory, output};
  auto display = std::make_shared<DisplayState>();
  auto surfaces = std::make_shared<SurfaceStore>();
  CoreSurfaceHle core_surface{registry, display, surfaces};
  OpenGlesHle gles{registry, display, surfaces};
  cpu.set_svc_handler([&](Cpu &source, std::uint32_t immediate) {
    require(registry.dispatch(source, 41, immediate),
            "OpenGLES firmware issued an unregistered HLE SVC");
  });
  std::size_t installed = 0;
  for (const auto &segment : image.segments()) {
    if (segment.file_size == 0)
      continue;
    installed +=
        registry.install_mapped_image(cpu, 41, *path, segment.vm_address,
                                      segment.file_size, segment.file_offset);
  }
  require(installed >= 180, "OpenGLES firmware exports were not intercepted");
  std::size_t core_installed = 0;
  for (const auto &segment : core_image.segments()) {
    if (segment.file_size == 0)
      continue;
    core_installed +=
        registry.install_mapped_image(cpu, 41, *core_path, segment.vm_address,
                                      segment.file_size, segment.file_offset);
  }
  require(core_installed != 0,
          "CoreSurface firmware exports were not intercepted for GLES");

  const auto invoke_image = [&](const MachOImage &target_image,
                                std::string_view symbol,
                                const std::vector<std::uint32_t> &arguments) {
    const auto *entry = target_image.find_symbol(symbol);
    require(entry != nullptr, "OpenGLES firmware symbol is missing");
    cpu.clear_halt();
    for (std::size_t index = 0; index < 4; ++index) {
      cpu.registers()[index] = index < arguments.size() ? arguments[index] : 0;
    }
    for (std::size_t index = 4; index < arguments.size(); ++index) {
      require(
          memory.write32(stack + static_cast<std::uint32_t>((index - 4U) * 4U),
                         arguments[index]),
          "OpenGLES HLE stack argument write failed");
    }
    cpu.registers()[13] = stack;
    cpu.registers()[14] = 0x1000U;
    cpu.registers()[15] = entry->value;
    cpu.set_cpsr(0x10);
    const auto result = cpu.step();
    require(result.exception.empty() && !result.fault,
            "OpenGLES firmware HLE entry faulted");
    return cpu.registers()[0];
  };
  const auto invoke = [&](std::string_view symbol,
                          const std::vector<std::uint32_t> &arguments) {
    return invoke_image(image, symbol, arguments);
  };

  const auto egl_display = invoke("_eglGetDisplay", {0});
  const auto egl_context = invoke("_eglCreateContext", {egl_display, 1, 0, 0});
  const auto egl_surface =
      invoke("_eglCreatePbufferSurface", {egl_display, 1, 0});
  require(egl_display != 0 && egl_context != 0 && egl_surface != 0 &&
              invoke("_eglMakeCurrent",
                     {egl_display, egl_surface, egl_surface, egl_context}) == 1,
          "OpenGLES test context setup failed");

  invoke("_glGenTextures", {1, output_address});
  const auto texture_name = memory.read32(output_address).value_or(0);
  require(texture_name != 0 && invoke("_glIsTexture", {texture_name}) == 0,
          "OpenGLES texture name generation failed");
  invoke("_glBindTexture", {gles_abi::texture_2d, texture_name});
  require(invoke("_glIsTexture", {texture_name}) == 1,
          "OpenGLES generated texture did not become an object on bind");
  invoke("_glPixelStorei", {gles_abi::unpack_alignment, 1});
  const std::array<std::byte, 16> rgba_pixels{
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
      std::byte{0x00}, std::byte{0x80}, std::byte{0x00}, std::byte{0x80},
      std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x00},
  };
  require(memory.copy_in(data_address, rgba_pixels),
          "OpenGLES RGBA source write failed");
  invoke("_glTexImage2D",
         {gles_abi::texture_2d, 0, gles_abi::rgba, 2, 2, 0, gles_abi::rgba,
          gles_abi::unsigned_byte, data_address});
  require(invoke("_glGetError", {}) == gles_abi::no_error,
          "OpenGLES RGBA upload reported an error");
  const auto *texture = gles.resources().texture(texture_name);
  require(texture != nullptr && texture->levels.contains(0) &&
              texture->levels.at(0).width == 2 &&
              texture->levels.at(0).height == 2 &&
              texture->levels.at(0).argb ==
                  std::vector<std::uint32_t>{0xffff0000U, 0x80008000U,
                                             0xff0000ffU, 0x00ffffffU},
          "OpenGLES RGBA upload conversion is incorrect");

  const std::array<std::byte, 4> bgra_pixel{std::byte{0x11}, std::byte{0x22},
                                            std::byte{0x33}, std::byte{0x44}};
  require(memory.copy_in(data_address, bgra_pixel),
          "OpenGLES BGRA source write failed");
  invoke("_glTexSubImage2D",
         {gles_abi::texture_2d, 0, 1, 0, 1, 1, gles_abi::bgra_apple,
          gles_abi::unsigned_byte, data_address});
  texture = gles.resources().texture(texture_name);
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              texture != nullptr &&
              texture->levels.at(0).argb[1] == 0x44332211U,
          "OpenGLES BGRA subimage update is incorrect");

  invoke("_glGenBuffers", {1, output_address + 4U});
  const auto buffer_name = memory.read32(output_address + 4U).value_or(0);
  require(buffer_name != 0 && invoke("_glIsBuffer", {buffer_name}) == 0,
          "OpenGLES buffer name generation failed");
  invoke("_glBindBuffer", {gles_abi::array_buffer, buffer_name});
  require(invoke("_glIsBuffer", {buffer_name}) == 1,
          "OpenGLES generated buffer did not become an object on bind");
  const std::array<std::byte, 8> buffer_data{
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
      std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
  require(memory.copy_in(data_address, buffer_data),
          "OpenGLES buffer source write failed");
  invoke("_glBufferData", {gles_abi::array_buffer,
                           static_cast<std::uint32_t>(buffer_data.size()),
                           data_address, gles_abi::static_draw});
  const std::array<std::byte, 2> buffer_patch{std::byte{0xaa}, std::byte{0xbb}};
  require(memory.copy_in(data_address, buffer_patch),
          "OpenGLES buffer patch write failed");
  invoke("_glBufferSubData", {gles_abi::array_buffer, 3, 2, data_address});
  invoke("_glGetBufferParameteriv",
         {gles_abi::array_buffer, gles_abi::buffer_size, output_address + 8U});
  const auto *buffer = gles.resources().buffer(buffer_name);
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              buffer != nullptr && buffer->bytes.size() == buffer_data.size() &&
              buffer->bytes[3] == std::byte{0xaa} &&
              buffer->bytes[4] == std::byte{0xbb} &&
              memory.read32(output_address + 8U) ==
                  std::optional<std::uint32_t>{buffer_data.size()},
          "OpenGLES buffer upload/subdata state is incorrect");

  const auto second_context =
      invoke("_eglCreateContext", {egl_display, 1, 0, 0});
  require(second_context != 0 &&
              invoke("_eglMakeCurrent", {egl_display, egl_surface, egl_surface,
                                         second_context}) == 1,
          "OpenGLES second context setup failed");
  invoke("_glTexImage2D",
         {gles_abi::texture_2d, 0, gles_abi::rgba, 1, 1, 0, gles_abi::rgba,
          gles_abi::unsigned_byte, data_address});
  require(invoke("_glGetError", {}) == gles_abi::invalid_operation,
          "OpenGLES texture binding leaked across contexts");

  require(invoke("_eglMakeCurrent",
                 {egl_display, egl_surface, egl_surface, egl_context}) == 1,
          "OpenGLES original context restore failed");
  constexpr std::uint32_t vertex_address = data_address + 0x100U;
  constexpr std::uint32_t color_address = data_address + 0x180U;
  const std::array<float, 6> triangle_vertices{80.0F,  120.0F, 240.0F,
                                               120.0F, 160.0F, 360.0F};
  for (std::size_t index = 0; index < triangle_vertices.size(); ++index) {
    require(
        memory.write32(vertex_address + static_cast<std::uint32_t>(index * 4U),
                       std::bit_cast<std::uint32_t>(triangle_vertices[index])),
        "OpenGLES triangle vertex write failed");
  }
  const std::array<std::byte, 12> triangle_colors{
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff}};
  require(memory.copy_in(color_address, triangle_colors),
          "OpenGLES triangle color write failed");
  invoke("_glBindBuffer", {gles_abi::array_buffer, buffer_name});
  invoke("_glBufferData",
         {gles_abi::array_buffer,
          static_cast<std::uint32_t>(triangle_vertices.size() * sizeof(float)),
          vertex_address, gles_abi::dynamic_draw});
  invoke("_glVertexPointer", {2, gles_abi::float_type, 0, 0});
  invoke("_glBindBuffer", {gles_abi::array_buffer, 0});
  invoke("_glColorPointer", {4, gles_abi::unsigned_byte, 0, color_address});
  invoke("_glEnableClientState", {gles_abi::vertex_array});
  invoke("_glEnableClientState", {gles_abi::color_array});
  invoke("_glMatrixMode", {gles_abi::projection});
  invoke("_glLoadIdentity", {});
  invoke("_glOrthof", {std::bit_cast<std::uint32_t>(0.0F),
                       std::bit_cast<std::uint32_t>(320.0F),
                       std::bit_cast<std::uint32_t>(0.0F),
                       std::bit_cast<std::uint32_t>(480.0F),
                       std::bit_cast<std::uint32_t>(-1.0F),
                       std::bit_cast<std::uint32_t>(1.0F)});
  invoke("_glMatrixMode", {gles_abi::modelview});
  invoke("_glLoadIdentity", {});
  invoke("_glViewport", {0, 0, default_display_width, default_display_height});
  display->clear(0xff000000U);
  invoke("_glDrawArrays", {gles_abi::triangles, 0, 3});
  const auto rendered = display->snapshot();
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              rendered.pixels[240U * default_display_width + 160U] ==
                  0xffff0000U,
          "OpenGLES VBO/client-array triangle rasterization failed");

  const auto surface_client =
      invoke_image(core_image, "_CoreSurfaceClientBufferCreate", {});
  const auto surface_base = invoke_image(
      core_image, "_CoreSurfaceClientBufferGetBaseAddress", {surface_client});
  const auto surface_id = invoke_image(
      core_image, "_CoreSurfaceClientBufferGetID", {surface_client});
  const auto surface_pitch = invoke_image(
      core_image, "_CoreSurfaceClientBufferGetBytesPerRow", {surface_client});
  constexpr std::uint32_t public_surface = output_address + 0x40U;
  require(surface_client != 0 && surface_base != 0 && surface_id != 0 &&
              surface_pitch == default_display_width *
                                   core_surface_abi::bytes_per_bgra_pixel &&
              memory.write32(public_surface +
                                 core_surface_abi::public_client_buffer_offset,
                             surface_client),
          "CoreSurface public wrapper setup for GLES failed");
  constexpr std::uint32_t sample_x = 7;
  constexpr std::uint32_t sample_y = 9;
  const auto sample_address = surface_base + sample_y * surface_pitch +
                              sample_x * core_surface_abi::bytes_per_bgra_pixel;
  constexpr std::uint32_t initial_surface_pixel = 0xff3366ccU;
  require(memory.write32(sample_address, initial_surface_pixel),
          "CoreSurface GLES source pixel write failed");

  invoke("_glGenTextures", {1, output_address + 0x10U});
  const auto surface_texture =
      memory.read32(output_address + 0x10U).value_or(0);
  invoke("_glBindTexture",
         {gles_abi::texture_rectangle_apple, surface_texture});
  invoke("_glTexImageCoreSurfaceAPPLE",
         {gles_abi::texture_rectangle_apple, public_surface});
  texture = gles.resources().texture(surface_texture);
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              texture != nullptr && texture->levels.contains(0) &&
              texture->levels.at(0).surface_id ==
                  std::optional<std::uint32_t>{surface_id} &&
              texture->levels.at(0).width == default_display_width &&
              texture->levels.at(0).height == default_display_height &&
              texture->levels.at(0).argb[sample_y * default_display_width +
                                         sample_x] == initial_surface_pixel,
          "OpenGLES CoreSurface rectangle texture import failed");

  constexpr std::uint32_t refreshed_surface_pixel = 0xff12ab34U;
  require(memory.write32(sample_address, refreshed_surface_pixel),
          "CoreSurface GLES refresh pixel write failed");
  invoke("_glFinishTextureAPPLE", {gles_abi::texture_rectangle_apple});
  texture = gles.resources().texture(surface_texture);
  invoke("_glGetIntegerv",
         {gles_abi::texture_binding_rectangle_apple, output_address + 0x14U});
  invoke("_glGetIntegerv",
         {gles_abi::maximum_texture_units, output_address + 0x18U});
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              texture != nullptr &&
              texture->levels.at(0).argb[sample_y * default_display_width +
                                         sample_x] == refreshed_surface_pixel &&
              memory.read32(output_address + 0x14U) ==
                  std::optional<std::uint32_t>{surface_texture} &&
              memory.read32(output_address + 0x18U) ==
                  std::optional<std::uint32_t>{
                      static_cast<std::uint32_t>(gles_abi::texture_unit_count)},
          "OpenGLES texture finish/query state failed");
  constexpr std::uint32_t texture_coordinate_address = data_address + 0x240U;
  const std::array<float, 6> texture_coordinates{
      static_cast<float>(sample_x), static_cast<float>(sample_y),
      static_cast<float>(sample_x), static_cast<float>(sample_y),
      static_cast<float>(sample_x), static_cast<float>(sample_y)};
  for (std::size_t index = 0; index < texture_coordinates.size(); ++index) {
    require(memory.write32(
                texture_coordinate_address +
                    static_cast<std::uint32_t>(index * sizeof(float)),
                std::bit_cast<std::uint32_t>(texture_coordinates[index])),
            "OpenGLES rectangle texture coordinate write failed");
  }
  invoke("_glDisableClientState", {gles_abi::color_array});
  invoke("_glColor4ub", {0, 0, 0, 0});
  invoke("_glGetFloatv", {gles_abi::current_color, output_address + 0x20U});
  invoke("_glTexEnvi", {gles_abi::texture_environment,
                        gles_abi::texture_environment_mode, gles_abi::replace});
  invoke("_glColorMask", {0, 1, 1, 1});
  invoke("_glTexCoordPointer",
         {2, gles_abi::float_type, 0, texture_coordinate_address});
  invoke("_glEnableClientState", {gles_abi::texture_coord_array});
  invoke("_glEnable", {gles_abi::texture_rectangle_apple});
  invoke("_glScissor", {0, 0, 10, 10});
  invoke("_glEnable", {gles_abi::scissor_test});
  display->clear(0xff000000U);
  invoke("_glDrawArrays", {gles_abi::triangles, 0, 3});
  require(display->snapshot().pixels[240U * default_display_width + 160U] ==
              0xff000000U,
          "OpenGLES scissor did not reject an out-of-box fragment");
  invoke("_glScissor", {100, 100, 120, 300});
  invoke("_glDrawArrays", {gles_abi::triangles, 0, 3});
  const auto surface_rendered = display->snapshot();
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              memory.read32(output_address + 0x20U) ==
                  std::optional<std::uint32_t>{
                      std::bit_cast<std::uint32_t>(0.0F)} &&
              surface_rendered.pixels[240U * default_display_width + 160U] ==
                  0xff00ab34U,
          "OpenGLES CoreSurface texture/scissor/color-mask state failed");
  invoke("_glClearColor", {std::bit_cast<std::uint32_t>(1.0F),
                           std::bit_cast<std::uint32_t>(1.0F),
                           std::bit_cast<std::uint32_t>(1.0F),
                           std::bit_cast<std::uint32_t>(1.0F)});
  invoke("_glClear", {gles_abi::color_buffer_bit});
  const auto cleared = display->snapshot();
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              cleared.pixels[240U * default_display_width + 160U] ==
                  0xff00ffffU &&
              cleared.pixels[0] == 0xff000000U,
          "OpenGLES clear did not honor scissor and color write mask");

  constexpr auto second_texture_unit = gles_abi::texture0 + 1U;
  constexpr std::uint32_t second_texture_pixel = 0x80ff8040U;
  constexpr std::uint32_t second_texture_data = data_address + 0x300U;
  constexpr std::uint32_t second_texture_coordinates = data_address + 0x320U;
  const std::array<std::byte, 4> second_rgba{std::byte{0xff}, std::byte{0x80},
                                             std::byte{0x40}, std::byte{0x80}};
  require(memory.copy_in(second_texture_data, second_rgba),
          "OpenGLES second texture pixel write failed");
  invoke("_glGenTextures", {1, output_address + 0x80U});
  const auto second_texture = memory.read32(output_address + 0x80U).value_or(0);
  invoke("_glActiveTexture", {second_texture_unit});
  invoke("_glBindTexture", {gles_abi::texture_2d, second_texture});
  invoke("_glTexImage2D",
         {gles_abi::texture_2d, 0, gles_abi::rgba, 1, 1, 0, gles_abi::rgba,
          gles_abi::unsigned_byte, second_texture_data});
  invoke("_glTexEnvi",
         {gles_abi::texture_environment, gles_abi::texture_environment_mode,
          gles_abi::modulate});
  invoke("_glEnable", {gles_abi::texture_2d});
  for (std::size_t index = 0; index < 6; ++index) {
    require(
        memory.write32(second_texture_coordinates +
                           static_cast<std::uint32_t>(index * sizeof(float)),
                       std::bit_cast<std::uint32_t>(0.0F)),
        "OpenGLES second-unit coordinate write failed");
  }
  invoke("_glClientActiveTexture", {second_texture_unit});
  invoke("_glTexCoordPointer",
         {2, gles_abi::float_type, 0, second_texture_coordinates});
  invoke("_glEnableClientState", {gles_abi::texture_coord_array});
  invoke("_glGetIntegerv", {gles_abi::active_texture, output_address + 0x84U});
  invoke("_glGetIntegerv",
         {gles_abi::client_active_texture, output_address + 0x88U});
  invoke("_glGetIntegerv",
         {gles_abi::texture_binding_2d, output_address + 0x8cU});
  invoke("_glDisable", {gles_abi::scissor_test});
  invoke("_glColorMask", {1, 1, 1, 1});
  display->clear(0xff000000U);
  invoke("_glDrawArrays", {gles_abi::triangles, 0, 3});
  const auto multiply_channel = [](std::uint32_t left, std::uint32_t right,
                                   std::uint32_t shift) {
    return (((left >> shift) & 0xffU) * ((right >> shift) & 0xffU) + 127U) /
           255U;
  };
  const auto combined_pixel =
      (multiply_channel(refreshed_surface_pixel, second_texture_pixel, 24U)
       << 24U) |
      (multiply_channel(refreshed_surface_pixel, second_texture_pixel, 16U)
       << 16U) |
      (multiply_channel(refreshed_surface_pixel, second_texture_pixel, 8U)
       << 8U) |
      multiply_channel(refreshed_surface_pixel, second_texture_pixel, 0U);
  const auto multitextured = display->snapshot();
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              memory.read32(output_address + 0x84U) ==
                  std::optional<std::uint32_t>{second_texture_unit} &&
              memory.read32(output_address + 0x88U) ==
                  std::optional<std::uint32_t>{second_texture_unit} &&
              memory.read32(output_address + 0x8cU) ==
                  std::optional<std::uint32_t>{second_texture} &&
              multitextured.pixels[240U * default_display_width + 160U] ==
                  combined_pixel,
          "OpenGLES two-unit texture state/composition failed");

  invoke("_glActiveTexture", {gles_abi::texture0});
  invoke("_glGetIntegerv",
         {gles_abi::texture_binding_rectangle_apple, output_address + 0x90U});
  require(invoke("_glGetError", {}) == gles_abi::no_error &&
              memory.read32(output_address + 0x90U) ==
                  std::optional<std::uint32_t>{surface_texture},
          "OpenGLES texture binding leaked between active units");
}

void mobile_framebuffer_firmware_boundary_test() {
  const std::array candidates{
      std::filesystem::path{
          "build/rootfs/System/Library/Frameworks/"
          "IOMobileFramebuffer.framework/IOMobileFramebuffer"},
      std::filesystem::path{
          "rootfs/System/Library/Frameworks/"
          "IOMobileFramebuffer.framework/IOMobileFramebuffer"},
  };
  const auto path = std::find_if(
      candidates.begin(), candidates.end(),
      [](const auto &candidate) { return std::filesystem::exists(candidate); });
  if (path == candidates.end())
    return;

  AddressSpace memory;
  const auto image = MachOImage::parse(*path);
  image.map_into(memory);
  const auto *default_surface =
      image.find_symbol("_IOMobileFramebufferGetLayerDefaultSurface");
  const auto *display_size =
      image.find_symbol("_IOMobileFramebufferGetDisplaySize");
  const auto *swap_begin = image.find_symbol("_IOMobileFramebufferSwapBegin");
  const auto *swap_set_layer =
      image.find_symbol("_IOMobileFramebufferSwapSetLayer");
  const auto *swap_end = image.find_symbol("_IOMobileFramebufferSwapEnd");
  const auto *vsync_source =
      image.find_symbol("_IOMobileFramebufferGetVSyncRunLoopSource");
  const auto *vsync_enable =
      image.find_symbol("_IOMobileFramebufferEnableVSyncNotifications");
  const auto *vsync_notify =
      image.find_symbol("_IOMobileFramebufferNotifyFunc");
  const auto *request_power =
      image.find_symbol("_IOMobileFramebufferRequestPowerChange");
  require(default_surface != nullptr && display_size != nullptr &&
              swap_begin != nullptr && swap_set_layer != nullptr &&
              swap_end != nullptr && vsync_source != nullptr &&
              vsync_enable != nullptr && vsync_notify != nullptr &&
              request_power != nullptr,
          "IOMobileFramebuffer firmware exports are missing");
  const auto default_surface_instruction =
      memory.read32(default_surface->value);
  const auto display_size_instruction = memory.read32(display_size->value);
  const auto vsync_source_instruction = memory.read32(vsync_source->value);
  const auto vsync_enable_instruction = memory.read32(vsync_enable->value);
  const auto vsync_notify_instruction = memory.read32(vsync_notify->value);
  const auto request_power_instruction = memory.read32(request_power->value);
  require(default_surface_instruction && display_size_instruction &&
              vsync_source_instruction && vsync_enable_instruction &&
              vsync_notify_instruction && request_power_instruction,
          "IOMobileFramebuffer firmware code is not mapped");

  Dynarmic::ExclusiveMonitor monitor{1};
  Cpu cpu{0, memory, monitor};
  std::ostringstream stream;
  Output output{stream};
  UserlandHleRegistry registry{memory, output};
  auto display = std::make_shared<DisplayState>();
  auto surfaces = std::make_shared<SurfaceStore>();
  MobileFramebufferHle framebuffer{registry, display, surfaces};
  cpu.set_svc_handler([&](Cpu &source, std::uint32_t immediate) {
    require(registry.dispatch(source, 1, immediate),
            "IOMobileFramebuffer firmware issued an unregistered HLE SVC");
  });
  std::size_t installed = 0;
  for (const auto &segment : image.segments()) {
    if (segment.file_size == 0)
      continue;
    installed +=
        registry.install_mapped_image(cpu, 1, *path, segment.vm_address,
                                      segment.file_size, segment.file_offset);
  }
  require(installed != 0,
          "IOMobileFramebuffer HLE functions were not installed");
  require(memory.read32(default_surface->value) == default_surface_instruction,
          "default-surface firmware path was replaced by userspace HLE");
  require(
      memory.read32(vsync_source->value) == vsync_source_instruction &&
          memory.read32(vsync_enable->value) == vsync_enable_instruction &&
          memory.read32(vsync_notify->value) == vsync_notify_instruction &&
          memory.read32(request_power->value) == request_power_instruction,
      "firmware VSync/IONotificationPort path was replaced by userspace HLE");
  require(memory.read32(display_size->value) != display_size_instruction,
          "display-size firmware hook was not installed");

  constexpr auto pixel_count =
      static_cast<std::size_t>(default_display_width) * default_display_height;
  constexpr auto allocation_size = static_cast<std::uint32_t>(
      pixel_count * core_surface_abi::bytes_per_bgra_pixel);
  constexpr auto pitch =
      default_display_width * core_surface_abi::bytes_per_bgra_pixel;
  const auto pixel_base =
      registry.allocate_data(allocation_size, AddressSpace::page_size);
  const auto client =
      registry.allocate_data(core_surface_abi::client_buffer_structure_size);
  const auto public_surface = registry.allocate_data(
      core_surface_abi::public_client_buffer_offset + sizeof(std::uint32_t));
  const auto swap_id_output = registry.allocate_data(sizeof(std::uint32_t));
  const auto stack =
      registry.allocate_data(AddressSpace::page_size, AddressSpace::page_size);
  constexpr std::uint32_t surface_id = 0x21436587U;
  constexpr std::uint32_t sample_x = 73;
  constexpr std::uint32_t sample_y = 211;
  constexpr std::uint32_t sample_argb = 0xff25a9e3U;
  const auto sample_address = pixel_base + sample_y * pitch +
                              sample_x * core_surface_abi::bytes_per_bgra_pixel;
  require(
      pixel_base != 0 && client != 0 && public_surface != 0 &&
          swap_id_output != 0 && stack != 0 &&
          memory.write32(client + core_surface_abi::client_identifier_offset,
                         surface_id) &&
          memory.write32(public_surface +
                             core_surface_abi::public_client_buffer_offset,
                         client) &&
          memory.write32(sample_address, sample_argb),
      "IOMobileFramebuffer CoreSurface fixture setup failed");
  require(surfaces->publish(memory,
                            SurfaceStore::Backing{
                                surface_id, pixel_base, allocation_size,
                                default_display_width, default_display_height,
                                pitch, surface_pixel_format_bgra}),
          "IOMobileFramebuffer surface publication failed");

  const auto invoke = [&](std::string_view symbol,
                          const std::vector<std::uint32_t> &arguments) {
    const auto *entry = image.find_symbol(symbol);
    require(entry != nullptr, "IOMobileFramebuffer firmware symbol is missing");
    cpu.clear_halt();
    for (std::size_t index = 0; index < 4; ++index) {
      cpu.registers()[index] = index < arguments.size() ? arguments[index] : 0;
    }
    for (std::size_t index = 4; index < arguments.size(); ++index) {
      require(
          memory.write32(stack + static_cast<std::uint32_t>((index - 4U) * 4U),
                         arguments[index]),
          "IOMobileFramebuffer stack argument write failed");
    }
    cpu.registers()[13] = stack;
    cpu.registers()[14] = 0x1000U;
    cpu.registers()[15] = entry->value;
    cpu.set_cpsr(0x10);
    const auto result = cpu.step();
    require(result.exception.empty() && !result.fault,
            "IOMobileFramebuffer firmware HLE entry faulted");
    return cpu.registers()[0];
  };
  constexpr std::uint32_t framebuffer_object = 0x1001U;
  constexpr std::uint32_t rgb_layer = 0;
  const auto float_bits = [](float value) {
    return std::bit_cast<std::uint32_t>(value);
  };
  require(
      invoke("_IOMobileFramebufferSwapBegin",
             {framebuffer_object, swap_id_output}) == iokit_abi::success &&
          memory.read32(swap_id_output) == std::optional<std::uint32_t>{1} &&
          invoke("_IOMobileFramebufferSwapSetLayer",
                 {framebuffer_object, rgb_layer, public_surface,
                  float_bits(0.0F), float_bits(0.0F),
                  float_bits(static_cast<float>(default_display_width)),
                  float_bits(static_cast<float>(default_display_height)),
                  float_bits(0.0F), float_bits(0.0F),
                  float_bits(static_cast<float>(default_display_width)),
                  float_bits(static_cast<float>(default_display_height)), 0}) ==
              iokit_abi::success &&
          invoke("_IOMobileFramebufferSwapEnd", {framebuffer_object, 1}) ==
              iokit_abi::success,
      "IOMobileFramebuffer layer swap transaction failed");
  const auto frame = display->snapshot();
  require(frame.sequence == 1 &&
              frame.pixels[static_cast<std::size_t>(sample_y) *
                               default_display_width +
                           sample_x] == sample_argb,
          "IOMobileFramebuffer did not present the CoreSurface pixels");

  constexpr std::uint32_t overlay_surface_id = 0x21436588U;
  constexpr std::uint32_t overlay_argb = 0x80800000U;
  const auto overlay_base =
      registry.allocate_data(core_surface_abi::bytes_per_bgra_pixel);
  const auto overlay_client =
      registry.allocate_data(core_surface_abi::client_buffer_structure_size);
  const auto overlay_public_surface = registry.allocate_data(
      core_surface_abi::public_client_buffer_offset + sizeof(std::uint32_t));
  require(overlay_base != 0 && overlay_client != 0 &&
              overlay_public_surface != 0 &&
              memory.write32(overlay_base, overlay_argb) &&
              memory.write32(overlay_client +
                                 core_surface_abi::client_identifier_offset,
                             overlay_surface_id) &&
              memory.write32(overlay_public_surface +
                                 core_surface_abi::public_client_buffer_offset,
                             overlay_client),
          "IOMobileFramebuffer overlay fixture setup failed");
  require(surfaces->publish(
              memory,
              SurfaceStore::Backing{overlay_surface_id, overlay_base,
                                    core_surface_abi::bytes_per_bgra_pixel, 1,
                                    1, core_surface_abi::bytes_per_bgra_pixel,
                                    surface_pixel_format_bgra}),
          "IOMobileFramebuffer overlay publication failed");
  constexpr std::uint32_t overlay_layer = 1;
  require(
      invoke("_IOMobileFramebufferSwapBegin",
             {framebuffer_object, swap_id_output}) == iokit_abi::success &&
          invoke("_IOMobileFramebufferSwapSetLayer",
                 {framebuffer_object, overlay_layer, overlay_public_surface,
                  float_bits(0.0F), float_bits(0.0F), float_bits(1.0F),
                  float_bits(1.0F), float_bits(static_cast<float>(sample_x)),
                  float_bits(static_cast<float>(sample_y)), float_bits(1.0F),
                  float_bits(1.0F), 0}) == iokit_abi::success &&
          invoke("_IOMobileFramebufferSwapEnd", {framebuffer_object}) ==
              iokit_abi::success,
      "IOMobileFramebuffer overlay swap transaction failed");
  const auto blend_channel = [](std::uint32_t source, std::uint32_t destination,
                                std::uint32_t inverse_alpha) {
    return std::min(255U, source + (destination * inverse_alpha + 127U) / 255U);
  };
  constexpr auto overlay_alpha = overlay_argb >> 24U;
  constexpr auto overlay_inverse_alpha = 255U - overlay_alpha;
  const auto overlay_expected =
      (blend_channel(overlay_alpha, sample_argb >> 24U, overlay_inverse_alpha)
       << 24U) |
      (blend_channel((overlay_argb >> 16U) & 0xffU,
                     (sample_argb >> 16U) & 0xffU, overlay_inverse_alpha)
       << 16U) |
      (blend_channel((overlay_argb >> 8U) & 0xffU, (sample_argb >> 8U) & 0xffU,
                     overlay_inverse_alpha)
       << 8U) |
      blend_channel(overlay_argb & 0xffU, sample_argb & 0xffU,
                    overlay_inverse_alpha);
  require(display->snapshot().pixels[static_cast<std::size_t>(sample_y) *
                                         default_display_width +
                                     sample_x] == overlay_expected,
          "IOMobileFramebuffer layer alpha composition is incorrect");
  require(invoke("_IOMobileFramebufferSwapBegin",
                 {framebuffer_object, swap_id_output}) == iokit_abi::success &&
              invoke("_IOMobileFramebufferSwapSetLayer",
                     {framebuffer_object, overlay_layer, 0}) ==
                  iokit_abi::success &&
              invoke("_IOMobileFramebufferSwapEnd", {framebuffer_object}) ==
                  iokit_abi::success &&
              display->snapshot().pixels[static_cast<std::size_t>(sample_y) *
                                             default_display_width +
                                         sample_x] == sample_argb,
          "IOMobileFramebuffer did not remove a cleared hardware layer");
}

void run_tests() {
  ilemu::test::run_surface_store_tests();
  core_surface_firmware_hle_test();
  mbx2d_surface_composition_test();
  opengles_resource_firmware_boundary_test();
  mobile_framebuffer_firmware_boundary_test();
}

} // namespace

int main() { return ilemu::test::run_suite("graphics", run_tests); }
