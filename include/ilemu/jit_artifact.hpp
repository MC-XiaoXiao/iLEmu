#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ilemu/arm_cpu_model.hpp"
#include "ilemu/content_identity.hpp"

namespace ilemu {

enum class JitHostIsa : std::uint8_t {
  Unknown,
  X86_64,
  Arm64,
};

// Every field that can change the generated block or its calling convention
// belongs in this key. In particular, no path or mtime participates in cache
// identity.
struct JitArtifactKey {
  ContentIdentity content_identity;
  ContentIdentity layout_identity;
  std::uint32_t guest_pc{};
  bool thumb{};
  ArmArchitectureVersion architecture{ArmArchitectureVersion::Armv6K};
  ArmCpuModelKind cpu_model{ArmCpuModelKind::Arm1176JzfS};
  std::uint32_t timing_model_version{};
  std::uint32_t guest_ticks_per_second{};
  std::uint32_t image_slide{};
  std::uint32_t hle_abi_version{};
  std::uint32_t backend_abi_version{};
  std::uint64_t codegen_options{};
  JitHostIsa host_isa{JitHostIsa::Unknown};
  std::uint64_t host_feature_mask{};
  std::uint32_t artifact_format_version{};

  friend bool operator==(const JitArtifactKey &, const JitArtifactKey &) =
      default;
};

struct JitArtifactKeyHash {
  [[nodiscard]] std::size_t operator()(
      const JitArtifactKey &key) const noexcept;
};

struct JitArtifactData {
  // This is a normalized, portable representation. It is deliberately not a
  // copy of Dynarmic's native code cache.
  std::vector<std::byte> normalized_ir;
  std::vector<std::uint64_t> relocation_targets;
  std::vector<std::uint64_t> exit_locations;
  std::uint32_t instruction_count{};
  // Translation timing is advisory metadata until a portable IR exporter is
  // available at the Dynarmic boundary. It is still useful for deciding
  // whether an entry was produced by a real translation rather than a
  // speculative profile hint.
  std::uint64_t translation_nanoseconds{};
};

struct JitArtifactLimits {
  // Zero means unbounded. The default resident target matches the initial
  // metadata budget; native code is still owned by Dynarmic's live cache.
  std::size_t resident_bytes{64U * 1024U * 1024U};
  std::size_t persistence_bytes{};
};

struct BlockArtifact {
  JitArtifactKey key;
  JitArtifactData data;
};

class JitArtifactStore {
public:
  explicit JitArtifactStore(
      std::filesystem::path persistence_path = {},
      JitArtifactLimits limits = {});
  ~JitArtifactStore();

  JitArtifactStore(const JitArtifactStore &) = delete;
  JitArtifactStore &operator=(const JitArtifactStore &) = delete;

  [[nodiscard]] std::shared_ptr<const BlockArtifact> find(
      const JitArtifactKey &key) const;
  [[nodiscard]] std::shared_ptr<const BlockArtifact> publish(
      JitArtifactKey key, JitArtifactData data);
  [[nodiscard]] std::size_t size() const;

  // Persistence is metadata-only and atomic. Loading malformed data leaves
  // the existing store unchanged and lets execution fall back to JIT.
  [[nodiscard]] bool load(const std::filesystem::path &path) noexcept;
  [[nodiscard]] bool save() const noexcept;
  [[nodiscard]] bool save(const std::filesystem::path &path) const noexcept;

private:
  struct ArtifactRecord {
    std::shared_ptr<const BlockArtifact> artifact;
    std::size_t serialized_bytes{};
    std::list<JitArtifactKey>::iterator lru_position;
  };
  using ArtifactMap =
      std::unordered_map<JitArtifactKey, ArtifactRecord, JitArtifactKeyHash>;

  void touch_locked(ArtifactMap::iterator iterator) const;
  void evict_until_fit_locked(std::size_t required_bytes);
  void insert_locked(std::shared_ptr<const BlockArtifact> artifact,
                     std::size_t serialized_bytes);

  mutable std::mutex mutex_;
  mutable ArtifactMap artifacts_;
  mutable std::list<JitArtifactKey> lru_;
  JitArtifactLimits limits_;
  std::size_t resident_bytes_{};
  std::filesystem::path persistence_path_;
};

// Per-process mutable execution state. CPU registers, AddressSpace and HLE
// tables remain owned by their existing runtime objects; this class only
// supplies context identity and writable link cells for immutable artifacts.
class ExecutionContext {
public:
  explicit ExecutionContext(std::uint32_t process_id);

  [[nodiscard]] std::uint64_t context_id() const noexcept {
    return context_id_;
  }
  [[nodiscard]] std::uint32_t process_id() const noexcept {
    return process_id_;
  }

  [[nodiscard]] std::size_t create_link_cell();
  void link(std::size_t cell, std::uint64_t target_token);
  void unlink(std::size_t cell);
  [[nodiscard]] std::uint64_t linked_target(std::size_t cell) const;

private:
  struct LinkCell {
    std::atomic<std::uint64_t> target_token{};
  };

  std::uint64_t context_id_{};
  std::uint32_t process_id_{};
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<LinkCell>> link_cells_;
};

} // namespace ilemu
