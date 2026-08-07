#include "ilemu/jit_artifact.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
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
    auto first = store.publish(
        key, ilemu::JitArtifactData{{std::byte{0xa1}, std::byte{0xb2}},
                                    {0x2000U}, {0x3000U}, 2U});
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
  }

  std::filesystem::remove_all(root, error);
  return 0;
}
