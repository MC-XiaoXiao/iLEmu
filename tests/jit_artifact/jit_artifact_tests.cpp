#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/jit_artifact.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

ilemu::ContentIdentity identity_for(std::string value) {
  return ilemu::sha256(std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(value.data()), value.size()});
}

ilemu::JitArtifactKey make_key() {
  ilemu::JitArtifactKey key;
  key.content_identity = identity_for("same executable");
  key.layout_identity = identity_for("fixed text layout");
  key.guest_pc = 0x1000U;
  key.thumb = true;
  key.architecture = ilemu::ArmArchitectureVersion::Armv7;
  key.cpu_model = ilemu::ArmCpuModelKind::CortexA8;
  key.timing_model_version = 1U;
  key.guest_ticks_per_second = 600'000'000U;
  key.image_slide = 0x2000U;
  key.hle_abi_version = 4U;
  key.backend_abi_version = 7U;
  key.codegen_options = 0x55aaU;
  key.host_isa = ilemu::JitHostIsa::X86_64;
  key.host_feature_mask = 0x1234U;
  key.artifact_format_version = 1U;
  return key;
}

void write_le32(std::vector<std::byte> &bytes, std::size_t offset,
                std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.at(offset + index) = static_cast<std::byte>(value >> (index * 8U));
  }
}

ilemu::JitHostIsa host_isa() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return ilemu::JitHostIsa::Arm64;
#elif defined(__x86_64__) || defined(_M_X64)
  return ilemu::JitHostIsa::X86_64;
#else
  return ilemu::JitHostIsa::Unknown;
#endif
}

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "ilemu-jit-artifact-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  const auto persistence = root / "artifacts.bin";

  const auto key = make_key();
  {
    ilemu::JitArtifactStore store{persistence};
    ilemu::JitArtifactData first_data{
        {std::byte{0xa1}, std::byte{0xb2}}, {0x2000U}, {0x3000U}, 2U,
        17U};
    auto first = store.publish(
        key, std::move(first_data));
    auto duplicate = store.publish(
        key, ilemu::JitArtifactData{{std::byte{0xff}}, {}, {}, 1U});
    if (first != duplicate || first->data.normalized_ir.size() != 2U ||
        store.size() != 1U) {
      std::cerr << "same artifact key was not deduplicated\n";
      return 1;
    }

    auto different_layout = key;
    different_layout.layout_identity = identity_for("slid text layout");
    auto second = store.publish(
        different_layout, ilemu::JitArtifactData{{std::byte{0xc3}}, {}, {}, 1U});
    if (second == first || store.size() != 2U) {
      std::cerr << "different image layout reused an artifact\n";
      return 1;
    }

    ilemu::ExecutionContext first_context{101U};
    ilemu::ExecutionContext second_context{202U};
    const auto first_cell = first_context.create_link_cell();
    const auto second_cell = second_context.create_link_cell();
    first_context.link(first_cell, 0x1111U);
    if (first_context.context_id() == second_context.context_id() ||
        first_context.linked_target(first_cell) != 0x1111U ||
        second_context.linked_target(second_cell) != 0U) {
      std::cerr << "execution-context link cells were not private\n";
      return 1;
    }
    first_context.unlink(first_cell);
    if (first_context.linked_target(first_cell) != 0U) {
      std::cerr << "artifact link cell was not safely unlinked\n";
      return 1;
    }
  }

  {
    ilemu::JitArtifactStore reloaded{persistence};
    const auto artifact = reloaded.find(key);
    if (!artifact || artifact->data.normalized_ir !=
                         std::vector<std::byte>{std::byte{0xa1}, std::byte{0xb2}}) {
      std::cerr << "portable artifact metadata did not reload\n";
      return 1;
    }
    if (artifact->data.translation_nanoseconds != 17U) {
      std::cerr << "translation metadata did not reload\n";
      return 1;
    }
  }

  {
    ilemu::JitArtifactLimits resident_limits;
    resident_limits.resident_bytes = 282U;
    ilemu::JitArtifactStore resident_limited{
        std::filesystem::path{}, resident_limits};
    auto first_limited_key = key;
    first_limited_key.guest_pc = 0x2000U;
    auto second_limited_key = key;
    second_limited_key.guest_pc = 0x3000U;
    auto third_limited_key = key;
    third_limited_key.guest_pc = 0x4000U;
    const auto first_published = resident_limited.publish(
        first_limited_key,
        ilemu::JitArtifactData{{std::byte{0x01}}, {}, {}, 1U});
    const auto second_published = resident_limited.publish(
        second_limited_key,
        ilemu::JitArtifactData{{std::byte{0x02}}, {}, {}, 1U});
    const auto initial_size = resident_limited.size();
    const auto second_initial_hit = resident_limited.find(second_limited_key);
    const auto first_initial_hit = resident_limited.find(first_limited_key);
    const auto third_published = resident_limited.publish(
        third_limited_key,
        ilemu::JitArtifactData{{std::byte{0x03}}, {}, {}, 1U});
    const auto final_size = resident_limited.size();
    const auto first_final_hit = resident_limited.find(first_limited_key);
    const auto second_final_hit = resident_limited.find(second_limited_key);
    const auto third_final_hit = resident_limited.find(third_limited_key);
    if (!first_published || !second_published || initial_size != 2U ||
        !first_initial_hit || !second_initial_hit || !third_published ||
        final_size != 2U || !first_final_hit || second_final_hit ||
        !third_final_hit) {
      std::cerr << "resident artifact LRU budget was not enforced: sizes "
                << initial_size << ", " << final_size << " hits "
                << static_cast<bool>(first_published) << " "
                << static_cast<bool>(second_published) << " "
                << static_cast<bool>(first_initial_hit) << " "
                << static_cast<bool>(second_initial_hit) << " "
                << static_cast<bool>(third_published) << " "
                << static_cast<bool>(first_final_hit) << " "
                << static_cast<bool>(second_final_hit) << " "
                << static_cast<bool>(third_final_hit) << "\n";
      return 1;
    }
    resident_limits.resident_bytes = 141U;
    ilemu::JitArtifactStore reloaded_limited{persistence, resident_limits};
    if (reloaded_limited.size() != 1U) {
      std::cerr << "resident artifact budget was ignored during load\n";
      return 1;
    }

    ilemu::JitArtifactLimits persistence_limits;
    persistence_limits.persistence_bytes = 1U;
    ilemu::JitArtifactStore persistence_limited{
        std::filesystem::path{}, persistence_limits};
    if (!persistence_limited.publish(
            key, ilemu::JitArtifactData{{std::byte{0x02}}, {}, {}, 1U})) {
      std::cerr << "persistence budget rejected an in-memory artifact\n";
      return 1;
    }
    const auto limited_path = root / "limited.bin";
    if (persistence_limited.save(limited_path) ||
        std::filesystem::exists(limited_path)) {
      std::cerr << "persistence artifact budget was not enforced\n";
      return 1;
    }
  }

  const auto code_path = root / "immutable-code.bin";
  std::vector<std::byte> code(ilemu::AddressSpace::page_size);
  write_le32(code, 0, 0xe3a00001U); // mov r0, #1
  write_le32(code, 4, 0xef000080U); // svc #0x80
  {
    std::ofstream stream{code_path, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char *>(code.data()),
                 static_cast<std::streamsize>(code.size()));
    if (!stream) {
      std::cerr << "could not create immutable code fixture\n";
      return 1;
    }
  }

  auto runtime_artifacts = std::make_shared<ilemu::JitArtifactStore>();
  ilemu::AddressSpace memory;
  constexpr std::uint32_t code_address = 0x4000U;
  if (!memory.map_file(code_address, ilemu::AddressSpace::page_size,
                       ilemu::MemoryPermission::Read |
                           ilemu::MemoryPermission::Execute,
                       code_path, 0)) {
    std::cerr << "could not map immutable code fixture\n";
    return 1;
  }
  const auto backing = memory.executable_backing_identity(code_address, 4);
  if (!backing) {
    std::cerr << "immutable code fixture has no backing identity\n";
    return 1;
  }

  Dynarmic::ExclusiveMonitor monitor{3};
  {
    ilemu::CpuCluster cluster{1, 1, memory, 1,
                              ilemu::default_arm_cpu_model(), monitor, 0,
                              runtime_artifacts};
    auto &cpu = cluster.cpu(0);
    cpu.registers()[15] = code_address;
    cpu.set_cpsr(0x10U);
    const auto result = cpu.run(16);
    if (!result.svc || *result.svc != 0x80U ||
        runtime_artifacts->size() == 0) {
      std::cerr << "CPU translation did not publish an executable artifact\n";
      return 1;
    }
  }

  {
    ilemu::AddressSpace second_memory;
    if (!second_memory.map_file(code_address, ilemu::AddressSpace::page_size,
                                ilemu::MemoryPermission::Read |
                                    ilemu::MemoryPermission::Execute,
                                code_path, 0)) {
      std::cerr << "could not map second immutable code fixture\n";
      return 1;
    }
    ilemu::CpuCluster second_cluster{
        1, 1, second_memory, 1, ilemu::default_arm_cpu_model(), monitor, 1,
        runtime_artifacts};
    auto &cpu = second_cluster.cpu(0);
    cpu.registers()[15] = code_address;
    cpu.set_cpsr(0x10U);
    const auto result = cpu.run(16);
    if (!result.svc || runtime_artifacts->size() != 1U) {
      std::cerr << "same executable content was not reused across processes\n";
      return 1;
    }
  }

  {
    ilemu::AddressSpace slid_memory;
    constexpr std::uint32_t slid_address = 0x8000U;
    if (!slid_memory.map_file(slid_address, ilemu::AddressSpace::page_size,
                              ilemu::MemoryPermission::Read |
                                  ilemu::MemoryPermission::Execute,
                              code_path, 0)) {
      std::cerr << "could not map slid immutable code fixture\n";
      return 1;
    }
    ilemu::CpuCluster slid_cluster{
        1, 1, slid_memory, 1, ilemu::default_arm_cpu_model(), monitor, 2,
        runtime_artifacts};
    auto &cpu = slid_cluster.cpu(0);
    cpu.registers()[15] = slid_address;
    cpu.set_cpsr(0x10U);
    const auto result = cpu.run(16);
    if (!result.svc || runtime_artifacts->size() != 2U) {
      std::cerr << "different executable layout reused an artifact\n";
      return 1;
    }
  }

  ilemu::JitArtifactKey expected;
  expected.content_identity = backing->content;
  expected.layout_identity = backing->layout;
  expected.guest_pc = code_address;
  expected.thumb = false;
  expected.architecture = ilemu::ArmArchitectureVersion::Armv6K;
  expected.cpu_model = ilemu::ArmCpuModelKind::Arm1176JzfS;
  expected.timing_model_version = 1U;
  expected.guest_ticks_per_second = 400'000'000U;
  expected.image_slide = 0U;
  expected.hle_abi_version = 1U;
  expected.backend_abi_version = 1U;
  expected.codegen_options = 1U;
  expected.host_isa = host_isa();
  expected.host_feature_mask = 0U;
  expected.artifact_format_version = 2U;
  const auto published = runtime_artifacts->find(expected);
  if (!published || published->data.normalized_ir.empty()) {
    std::cerr << "published artifact key did not match executable backing\n";
    return 1;
  }

  std::filesystem::remove_all(root, error);
  return 0;
}
