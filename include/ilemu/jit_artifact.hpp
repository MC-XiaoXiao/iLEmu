#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
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
  // Dynarmic's complete block location. guest_pc/thumb remain as readable
  // fields for diagnostics, but this value is the cache identity for all
  // CPSR/FPSCR/IT/single-step state that affects translation.
  std::uint64_t location_descriptor{};
  ArmArchitectureVersion architecture{ArmArchitectureVersion::Armv6K};
  ArmCpuModelKind cpu_model{ArmCpuModelKind::Arm1176JzfS};
  std::uint32_t timing_model_version{};
  std::uint32_t guest_ticks_per_second{};
  std::uint32_t image_slide{};
  std::uint32_t hle_abi_version{};
  std::uint32_t backend_abi_version{};
  std::uint64_t dynarmic_build_fingerprint{};
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

struct JitCodeDependency {
  std::uint32_t address{};
  std::uint32_t size{};
  ContentIdentity content_identity;
  ContentIdentity layout_identity;
};

struct JitConstantDependency {
  std::uint32_t address{};
  std::uint32_t size{};
  std::uint64_t value{};
  ContentIdentity content_identity;
  ContentIdentity layout_identity;
};

struct JitArtifactData {
  // This is a normalized, portable representation. It is deliberately not a
  // copy of Dynarmic's native code cache.
  std::vector<std::byte> normalized_ir;
  std::vector<std::uint64_t> relocation_targets;
  std::vector<std::uint64_t> exit_locations;
  std::uint32_t instruction_count{};
  // Translation timing is advisory metadata. It is useful for deciding
  // whether an entry was produced by a real translation rather than a
  // speculative profile hint, but it does not participate in executable
  // artifact identity.
  std::uint64_t translation_nanoseconds{};
  // Every executable page observed by Dynarmic while translating the block.
  // Empty dependencies are not importable by the CPU integration.
  std::vector<JitCodeDependency> code_dependencies;
  // Read-only values folded by Dynarmic's constant-memory pass.
  std::vector<JitConstantDependency> constant_dependencies;
};

struct JitArtifactLimits {
  // Zero means unbounded. The default resident target matches the initial
  // metadata budget; native code is still owned by Dynarmic's live cache.
  std::size_t resident_bytes{64U * 1024U * 1024U};
  std::size_t persistence_bytes{};
  // A disabled persistence store is a successful no-op on save and a cache
  // miss on load. This lets the host disable optional storage under pressure
  // without affecting Guest execution.
  bool persistence_enabled{true};
  // Zero disables the filesystem free-space safety check.
  std::uintmax_t minimum_free_bytes{};
  // A journal at or above this size is eligible for low-priority compaction.
  // Zero disables automatic compaction.
  std::size_t compaction_bytes{64U * 1024U * 1024U};
  // Newly translated artifacts enter this bounded queue before the resident
  // LRU can evict them. Zero disables asynchronous writeback.
  std::size_t writeback_bytes{16U * 1024U * 1024U};
};

struct JitArtifactStoreStats {
  std::uint64_t lookups{};
  std::uint64_t memory_hits{};
  std::uint64_t disk_hits{};
  std::uint64_t disk_read_retries{};
  std::uint64_t disk_read_waits{};
  std::uint64_t misses{};
  std::uint64_t publish_calls{};
  std::uint64_t deduplicated_publishes{};
  std::uint64_t disk_loaded_entries{};
  std::uint64_t evictions{};
  std::uint64_t compactions{};
  std::uint64_t writeback_enqueued{};
  std::uint64_t writeback_saved{};
  std::uint64_t writeback_dropped{};
  std::uint64_t writeback_failures{};
  std::uint64_t writeback_cancellations{};
  std::size_t resident_bytes{};
  std::size_t writeback_pending_bytes{};
  std::uintmax_t disk_bytes{};
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
  [[nodiscard]] JitArtifactStoreStats stats() const;
  // Stops optional background persistence and releases queued artifacts.
  // Guest execution and resident-cache lookups remain available.
  void cancel_writeback() noexcept;
  [[nodiscard]] bool compaction_needed() const noexcept;
  [[nodiscard]] bool compact() const noexcept;

  // Persistence is metadata-only. Initial snapshots are atomic; incremental
  // publications use a checksummed append journal. Loading malformed data
  // leaves the existing store unchanged and lets execution fall back to JIT.
  [[nodiscard]] bool load(const std::filesystem::path &path) noexcept;
  [[nodiscard]] bool save() const noexcept;
  [[nodiscard]] bool save(const std::filesystem::path &path) const noexcept;

private:
  struct DiskArtifactRecord {
    std::uint64_t offset{};
    std::uint64_t serialized_bytes{};
    bool append_log{};
    std::uint64_t generation{};
  };
  struct ArtifactRecord {
    std::shared_ptr<const BlockArtifact> artifact;
    std::size_t serialized_bytes{};
    std::list<JitArtifactKey>::iterator lru_position;
    bool loaded_from_disk{};
  };
  struct PendingWriteback {
    std::shared_ptr<const BlockArtifact> artifact;
    std::size_t serialized_bytes{};
    std::list<JitArtifactKey>::iterator queue_position;
  };
  struct DiskReadFlight {
    std::condition_variable condition;
    std::shared_ptr<const BlockArtifact> artifact;
    bool complete{};
    bool disk_hit{};
  };
  using ArtifactMap =
      std::unordered_map<JitArtifactKey, ArtifactRecord, JitArtifactKeyHash>;
  using DiskArtifactMap = std::unordered_map<JitArtifactKey,
                                             DiskArtifactRecord,
                                             JitArtifactKeyHash>;
  using PendingWritebackMap =
      std::unordered_map<JitArtifactKey, PendingWriteback,
                         JitArtifactKeyHash>;
  using DiskReadFlightMap =
      std::unordered_map<JitArtifactKey, std::shared_ptr<DiskReadFlight>,
                         JitArtifactKeyHash>;
  enum class AppendResult : std::uint8_t {
    NotApplicable,
    Saved,
    Failed,
  };

  void touch_locked(ArtifactMap::iterator iterator) const;
  [[nodiscard]] std::uint64_t next_disk_generation_locked() const noexcept;
  void evict_until_fit_locked(std::size_t required_bytes) const;
  void insert_locked(std::shared_ptr<const BlockArtifact> artifact,
                     std::size_t serialized_bytes,
                     bool loaded_from_disk = false) const;
  [[nodiscard]] bool enqueue_writeback_locked(
      const std::shared_ptr<const BlockArtifact> &artifact,
      std::size_t serialized_bytes) const;
  void retire_writeback_locked(const JitArtifactKey &key) const;
  void writeback_loop();
  [[nodiscard]] bool append_writeback_batch(
      const std::vector<std::shared_ptr<const BlockArtifact>> &batch) const
      noexcept;
  [[nodiscard]] AppendResult append_new_artifacts(
      const std::filesystem::path &path) const noexcept;
  [[nodiscard]] bool save_full(
      const std::filesystem::path &path) const noexcept;

  mutable std::mutex mutex_;
  mutable ArtifactMap artifacts_;
  mutable std::list<JitArtifactKey> lru_;
  mutable PendingWritebackMap pending_writebacks_;
  mutable std::list<JitArtifactKey> writeback_order_;
  mutable DiskReadFlightMap disk_read_flights_;
  mutable DiskArtifactMap disk_artifacts_;
  mutable std::vector<JitArtifactKey> disk_order_;
  JitArtifactLimits limits_;
  mutable std::size_t resident_bytes_{};
  mutable std::size_t pending_writeback_bytes_{};
  std::filesystem::path persistence_path_;
  mutable std::filesystem::path disk_source_path_;
  mutable std::filesystem::path disk_append_path_;
  mutable std::uint64_t disk_append_valid_bytes_{};
  mutable std::uint64_t disk_index_generation_{};
  mutable std::mutex persistence_mutex_;
  mutable std::condition_variable writeback_condition_;
  mutable bool writeback_stopping_{};
  mutable bool writeback_disabled_{};
  mutable std::atomic<bool> writeback_cancel_requested_{};
  std::thread writeback_thread_;
  mutable JitArtifactStoreStats stats_;
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
