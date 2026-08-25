#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ilemu/content_identity.hpp"

namespace ilemu {

inline constexpr std::size_t jit_translation_profile_maximum_locations = 32'768;

// A recorder belongs to one JIT executor. Its arrays are allocated with the
// executor, so ordinary demand translation never allocates or contends on the
// profile's merge mutex. The hash table is deliberately larger than the
// descriptor array so a full recorder still has an empty probe slot.
inline constexpr std::size_t jit_translation_profile_recorder_capacity = 4'096;
inline constexpr std::size_t jit_translation_profile_recorder_hash_capacity =
    8'192;

inline constexpr std::size_t jit_translation_profile_maximum_profiles = 256;
inline constexpr std::size_t jit_translation_profile_maximum_file_bytes =
    1U * 1024U * 1024U;
inline constexpr std::size_t jit_translation_profile_maximum_storage_bytes =
    16U * 1024U * 1024U;

enum class JitTranslationProfileRecordResult : std::uint8_t {
    Recorded,
    Deduplicated,
    DroppedCapacity,
    Ignored,
};

// Fixed-storage, executor-local hot-path recorder. The returned spans remain
// valid until reset() and are consumed only at a guest safe point.
class JitTranslationProfileRecorder {
public:
    JitTranslationProfileRecorder() noexcept = default;

    [[nodiscard]] JitTranslationProfileRecordResult record(
        std::uint64_t location_descriptor) noexcept;
    [[nodiscard]] std::span<const std::uint64_t> locations() const noexcept
    {
        return { locations_.data(), size_ };
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t deduplicated() const noexcept
    {
        return deduplicated_;
    }
    [[nodiscard]] std::uint64_t dropped_capacity() const noexcept
    {
        return dropped_capacity_;
    }
    void reset() noexcept;

private:
    [[nodiscard]] static std::size_t hash(
        std::uint64_t location_descriptor) noexcept;

    std::array<std::uint64_t, jit_translation_profile_recorder_capacity>
        locations_ { };
    std::array<std::uint64_t, jit_translation_profile_recorder_hash_capacity>
        known_locations_ { };
    std::size_t size_ { };
    std::uint64_t deduplicated_ { };
    std::uint64_t dropped_capacity_ { };
};

struct JitTranslationProfileStats {
    // The legacy names remain in the structure for callers compiled against
    // the Stage 4.2 interface. Reports use the explicit names below so a
    // disk load is never confused with descriptors recorded during this run.
    std::uint64_t recorded { };
    std::uint64_t recorded_descriptors { };
    std::uint64_t newly_recorded_descriptors { };
    std::uint64_t deduplicated { };
    std::uint64_t recorder_deduplicated { };
    std::uint64_t recorder_dropped_capacity { };
    std::uint64_t dropped_capacity { };
    std::uint64_t unstable_dropped { };
    std::uint64_t profile_loaded { };
    std::uint64_t disk_descriptors_loaded { };
    std::uint64_t profile_files_loaded { };
    std::uint64_t disk_files_loaded { };
    std::uint64_t profile_enqueued_portable { };
    std::uint64_t profile_native_enqueued { };
    std::uint64_t profile_native_attempted { };
    std::uint64_t profile_native_executed { };
    std::uint64_t profile_portable_attempted { };
    std::uint64_t profile_portable_executed { };
    std::uint64_t profile_portable_generated { };
    std::uint64_t portable_existence_hits { };
    std::uint64_t native_preimport_attempted { };
    std::uint64_t native_preimport_imported { };
    std::uint64_t native_preimport_already_present { };
    std::uint64_t native_preimport_before_first_demand { };
    std::uint64_t native_preimport_used { };
    std::uint64_t native_preimport_first_use_distance_samples { };
    std::uint64_t native_preimport_first_use_distance_total { };
    std::uint64_t demand_artifact_staged { };
    std::uint64_t demand_artifact_consumed { };
    std::uint64_t profile_portable_artifact_consumed { };
    std::uint64_t ordinary_demand_artifact_consumed { };
    std::uint64_t demand_artifact_stage_unused { };
    std::uint64_t profile_imported_before_first_run { };
    std::uint64_t merge_calls { };
    std::uint64_t merge_nanoseconds { };
    std::uint64_t save_calls { };
    std::uint64_t save_nanoseconds { };
    std::uint64_t profile_save_failures { };
    std::uint64_t load_nanoseconds { };
    std::uint64_t profile_bytes { };
    std::size_t profile_object_bytes { };
    std::size_t location_vector_bytes { };
    std::size_t known_set_bucket_bytes { };
    std::size_t known_set_node_bytes { };
    std::size_t discarded_set_bucket_bytes { };
    std::size_t discarded_set_node_bytes { };
    std::size_t portable_ready_set_bucket_bytes { };
    std::size_t portable_ready_set_node_bytes { };
    std::size_t resident_bytes { };
};

// A process-image profile contains only complete A32 location descriptors that
// previously reached successful host-code emission. It does not own or share
// generated machine code, callbacks, page tables, or guest memory.
class JitTranslationProfile {
public:
    JitTranslationProfile() = default;
    explicit JitTranslationProfile(
        std::vector<std::uint64_t> location_descriptors);

    // Profiling is an optional bounded working-set optimization and must
    // never interrupt guest translation if host memory is constrained.
    void record(std::uint64_t location_descriptor) noexcept;
    // Merge a recorder batch at a Guest safe point. Recorder-local duplicate
    // and overflow counts are supplied so all capacity decisions remain
    // visible without adding hot-path atomics.
    void merge(std::span<const std::uint64_t> location_descriptors,
        std::uint64_t recorder_deduplicated = 0,
        std::uint64_t recorder_dropped_capacity = 0) noexcept;
    // A hint can become invalid when a prior run's slid image occupied the same
    // executable address. Discarding is advisory and takes effect when the
    // profile is next saved; guest execution never depends on the hint.
    void discard(std::uint64_t location_descriptor) noexcept;
    [[nodiscard]] std::vector<std::uint64_t> snapshot() const;
    [[nodiscard]] std::pair<std::vector<std::uint64_t>, std::size_t>
    snapshot_range(std::size_t offset, std::size_t maximum) const;
    [[nodiscard]] std::size_t storage_size() const noexcept;
    [[nodiscard]] JitTranslationProfileStats stats() const noexcept;

    void note_profile_loaded(std::uint64_t descriptors) noexcept;
    void note_profile_enqueued_portable(std::uint64_t count = 1) noexcept;
    void note_profile_native_enqueued(std::uint64_t count = 1) noexcept;
    void note_profile_native_attempted() noexcept;
    void note_profile_native_executed() noexcept;
    void note_profile_portable_attempted() noexcept;
    void note_profile_portable_executed() noexcept;
    void note_portable_existence_hit() noexcept;
    void note_profile_portable_generated() noexcept;
    void note_profile_portable_artifact_ready(
        std::uint64_t location_descriptor) noexcept;
    void note_native_preimport_attempted() noexcept;
    void note_native_preimport_before_first_demand() noexcept;
    void note_native_preimport_imported() noexcept;
    void note_native_preimport_already_present() noexcept;
    void note_native_preimport_used(
        std::uint64_t first_use_distance = 0U) noexcept;
    void note_demand_artifact_staged() noexcept;
    void note_demand_artifact_consumed() noexcept;
    [[nodiscard]] bool consume_profile_portable_artifact(
        std::uint64_t location_descriptor) noexcept;
    void note_ordinary_demand_artifact_consumed() noexcept;
    void note_demand_artifact_stage_unused() noexcept;
    void note_profile_imported_before_first_run() noexcept;
    void note_unstable_dropped(std::uint64_t count = 1) noexcept;
    void note_save(std::uint64_t nanoseconds, std::uint64_t bytes) noexcept;
    void note_save_failure() noexcept;
    void note_profile_bytes(std::uint64_t bytes) noexcept;
    void note_load(std::uint64_t nanoseconds) noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<std::uint64_t> locations_;
    std::unordered_set<std::uint64_t> known_locations_;
    std::unordered_set<std::uint64_t> discarded_locations_;
    std::atomic<std::uint64_t> recorded_ { };
    std::atomic<std::uint64_t> deduplicated_ { };
    std::atomic<std::uint64_t> recorder_deduplicated_ { };
    std::atomic<std::uint64_t> recorder_dropped_capacity_ { };
    std::atomic<std::uint64_t> dropped_capacity_ { };
    std::atomic<std::uint64_t> unstable_dropped_ { };
    std::atomic<std::uint64_t> profile_loaded_ { };
    std::atomic<std::uint64_t> profile_files_loaded_ { };
    std::atomic<std::uint64_t> profile_enqueued_portable_ { };
    std::atomic<std::uint64_t> profile_native_enqueued_ { };
    std::atomic<std::uint64_t> profile_native_attempted_ { };
    std::atomic<std::uint64_t> profile_native_executed_ { };
    std::atomic<std::uint64_t> profile_portable_attempted_ { };
    std::atomic<std::uint64_t> profile_portable_executed_ { };
    std::atomic<std::uint64_t> profile_portable_generated_ { };
    std::atomic<std::uint64_t> portable_existence_hits_ { };
    std::atomic<std::uint64_t> native_preimport_attempted_ { };
    std::atomic<std::uint64_t> native_preimport_imported_ { };
    std::atomic<std::uint64_t> native_preimport_already_present_ { };
    std::atomic<std::uint64_t> native_preimport_before_first_demand_ { };
    std::atomic<std::uint64_t> native_preimport_used_ { };
    std::atomic<std::uint64_t> native_preimport_first_use_distance_samples_ { };
    std::atomic<std::uint64_t> native_preimport_first_use_distance_total_ { };
    std::atomic<std::uint64_t> demand_artifact_staged_ { };
    std::atomic<std::uint64_t> demand_artifact_consumed_ { };
    std::atomic<std::uint64_t> profile_portable_artifact_consumed_ { };
    std::atomic<std::uint64_t> ordinary_demand_artifact_consumed_ { };
    std::atomic<std::uint64_t> demand_artifact_stage_unused_ { };
    std::atomic<std::uint64_t> profile_imported_before_first_run_ { };
    std::atomic<std::uint64_t> merge_calls_ { };
    std::atomic<std::uint64_t> merge_nanoseconds_ { };
    std::atomic<std::uint64_t> save_calls_ { };
    std::atomic<std::uint64_t> save_nanoseconds_ { };
    std::atomic<std::uint64_t> load_nanoseconds_ { };
    std::atomic<std::uint64_t> profile_bytes_ { };
    std::atomic<std::uint64_t> profile_save_failures_ { };
    std::unordered_set<std::uint64_t> profile_portable_artifact_locations_;
};

// Profiles are host cache hints stored outside the guest root filesystem.
// Files contain descriptors, never generated host machine code. The exact
// executable content identity, rather than a path or mtime, selects a file.
// Invalid, stale, or truncated files are ignored and replaced after a clean
// simulator shutdown.
class JitTranslationProfileStore {
public:
    explicit JitTranslationProfileStore(
        std::filesystem::path data_directory, bool save_enabled = true);
    ~JitTranslationProfileStore();

    JitTranslationProfileStore(const JitTranslationProfileStore&) = delete;
    JitTranslationProfileStore& operator=(
        const JitTranslationProfileStore&) = delete;

    [[nodiscard]] std::shared_ptr<JitTranslationProfile> profile_for(
        const ContentIdentity& executable_identity, bool load_from_disk = true);
    void save() noexcept;
    [[nodiscard]] JitTranslationProfileStats stats() const noexcept;

private:
    std::filesystem::path data_directory_;
    std::map<ContentIdentity, std::shared_ptr<JitTranslationProfile>> profiles_;
    std::map<ContentIdentity, std::uint64_t> profile_access_order_;
    std::map<ContentIdentity, std::size_t> known_profile_bytes_;
    std::uint64_t next_access_order_ { 1 };
    std::size_t known_storage_bytes_ { };
    std::uint64_t profile_loads_ { };
    std::uint64_t profile_save_failures_ { };
    bool save_enabled_ { true };
};

} // namespace ilemu
