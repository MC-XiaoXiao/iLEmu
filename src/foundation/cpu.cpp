#include "ilemu/cpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/ir/basic_block.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/backend/x64/a32_jitstate.h>
#include <dynarmic/backend/x64/exclusive_monitor_friend.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "ilemu/jit_translation_profile.hpp"
#include "ilemu/jit_artifact.hpp"
#include "dynarmic_ir_artifact.hpp"
#include "ilemu/performance.hpp"

namespace ilemu {
namespace {

[[nodiscard]] Dynarmic::A32::ArchVersion dynarmic_architecture_version(
    ArmArchitectureVersion version) {
    switch (version) {
    case ArmArchitectureVersion::Armv6K:
        return Dynarmic::A32::ArchVersion::v6K;
    case ArmArchitectureVersion::Armv7:
        return Dynarmic::A32::ArchVersion::v7;
    }
    throw std::invalid_argument{"unsupported ARM architecture version"};
}

template <typename Jit>
std::uint64_t jit_code_cache_used(const Jit& jit) {
    if constexpr (requires { jit.CodeCacheUsed(); }) {
        return static_cast<std::uint64_t>(jit.CodeCacheUsed());
    }
    return 0;
}

constexpr std::size_t jit_link_cell_count = 9U;
constexpr std::size_t fast_dispatch_table_size = 0x10000U;
constexpr std::size_t fast_dispatch_entry_bytes = sizeof(std::uint64_t) * 2U;
constexpr std::size_t host_code_page_size = 4096U;
constexpr auto default_host_cooperative_slice_budget =
    std::chrono::milliseconds{2};
constexpr std::uint32_t host_yield_initial_check_interval = 32U;
constexpr std::uint32_t host_yield_min_check_interval = 4U;
constexpr std::uint32_t host_yield_max_check_interval = 64U;
constexpr std::uint64_t host_yield_initial_tick_budget = 2048U;
constexpr std::uint64_t host_yield_min_tick_budget = 256U;
constexpr std::uint64_t host_yield_max_tick_budget = 8192U;
constexpr auto host_yield_urgent_window = std::chrono::microseconds{250};
constexpr auto host_yield_slow_translation_threshold =
    std::chrono::microseconds{250};
constexpr std::size_t jit_native_preimport_tracker_capacity = 4'096U;
constexpr std::size_t jit_native_preimport_tracker_hash_capacity = 8'192U;
constexpr std::size_t jit_demand_seen_tracker_capacity = 32'768U;
constexpr std::size_t jit_demand_seen_tracker_hash_capacity = 65'536U;
static_assert(sizeof(void *) == sizeof(std::uint64_t));

// Native pre-import is performed by whichever executor owns the next
// precompile slot, while Guest execution may enter through another executor.
// Keep the correlation evidence in one bounded, lock-free tracker per
// execution pool rather than in one executor-local set.
class JitNativePreimportTracker {
public:
    JitNativePreimportTracker() noexcept { clear(); }

    void mark(std::uint64_t location_descriptor) noexcept {
        if (location_descriptor == 0U ||
            ready_count_.load(std::memory_order_acquire) >=
                jit_native_preimport_tracker_capacity) {
            return;
        }
        auto slot = hash(location_descriptor) &
                    (jit_native_preimport_tracker_hash_capacity - 1U);
        for (std::size_t probe = 0;
             probe < jit_native_preimport_tracker_hash_capacity; ++probe) {
            auto known = locations_[slot].load(std::memory_order_acquire);
            if (known == location_descriptor) return;
            if (known == 0U && locations_[slot].compare_exchange_weak(
                                  known, location_descriptor,
                                  std::memory_order_acq_rel,
                                  std::memory_order_acquire)) {
                ready_count_.fetch_add(1U, std::memory_order_release);
                return;
            }
            slot = (slot + 1U) &
                   (jit_native_preimport_tracker_hash_capacity - 1U);
        }
    }

    [[nodiscard]] bool has_ready() const noexcept {
        return ready_count_.load(std::memory_order_acquire) != 0U;
    }

    void mark_demand_seen(std::uint64_t location_descriptor) noexcept {
        if (location_descriptor == 0U ||
            demand_seen_count_.load(std::memory_order_acquire) >=
                jit_demand_seen_tracker_capacity) {
            return;
        }
        auto slot = hash(location_descriptor) &
                    (jit_demand_seen_tracker_hash_capacity - 1U);
        for (std::size_t probe = 0;
             probe < jit_demand_seen_tracker_hash_capacity; ++probe) {
            auto known = demand_locations_[slot].load(
                std::memory_order_acquire);
            if (known == location_descriptor) return;
            if (known == 0U && demand_locations_[slot].compare_exchange_weak(
                                  known, location_descriptor,
                                  std::memory_order_acq_rel,
                                  std::memory_order_acquire)) {
                demand_seen_count_.fetch_add(1U, std::memory_order_release);
                return;
            }
            slot = (slot + 1U) &
                   (jit_demand_seen_tracker_hash_capacity - 1U);
        }
    }

    [[nodiscard]] bool demand_seen(
        std::uint64_t location_descriptor) const noexcept {
        if (location_descriptor == 0U) return false;
        auto slot = hash(location_descriptor) &
                    (jit_demand_seen_tracker_hash_capacity - 1U);
        for (std::size_t probe = 0;
             probe < jit_demand_seen_tracker_hash_capacity; ++probe) {
            const auto known = demand_locations_[slot].load(
                std::memory_order_acquire);
            if (known == 0U) return false;
            if (known == location_descriptor) return true;
            slot = (slot + 1U) &
                   (jit_demand_seen_tracker_hash_capacity - 1U);
        }
        return false;
    }

    [[nodiscard]] bool consume(std::uint64_t location_descriptor) noexcept {
        if (location_descriptor == 0U) return false;
        auto slot = hash(location_descriptor) &
                    (jit_native_preimport_tracker_hash_capacity - 1U);
        for (std::size_t probe = 0;
             probe < jit_native_preimport_tracker_hash_capacity; ++probe) {
            auto known = locations_[slot].load(std::memory_order_acquire);
            if (known == 0U) return false;
            if (known == location_descriptor && locations_[slot].compare_exchange_weak(
                                                    known, 0U,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
                ready_count_.fetch_sub(1U, std::memory_order_release);
                return true;
            }
            slot = (slot + 1U) &
                   (jit_native_preimport_tracker_hash_capacity - 1U);
        }
        return false;
    }

    void clear() noexcept {
        for (auto& location : locations_) {
            location.store(0U, std::memory_order_release);
        }
        ready_count_.store(0U, std::memory_order_release);
        clear_demand_locations();
    }

    void clear_demand_locations() noexcept {
        for (auto& location : demand_locations_) {
            location.store(0U, std::memory_order_release);
        }
        demand_seen_count_.store(0U, std::memory_order_release);
    }

private:
    [[nodiscard]] static std::size_t hash(
        std::uint64_t location_descriptor) noexcept {
        auto value = location_descriptor;
        value ^= value >> 30U;
        value *= 0xbf58476d1ce4e5b9ULL;
        value ^= value >> 27U;
        value *= 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        return static_cast<std::size_t>(value);
    }

    std::array<std::atomic<std::uint64_t>,
               jit_native_preimport_tracker_hash_capacity>
        locations_{};
    std::atomic<std::uint64_t> ready_count_{};
    std::array<std::atomic<std::uint64_t>,
               jit_demand_seen_tracker_hash_capacity>
        demand_locations_{};
    std::atomic<std::uint64_t> demand_seen_count_{};
};

class CpuRunPhaseDiagnostics {
public:
    static constexpr auto first_kind =
        static_cast<std::size_t>(PerfLatencyKind::CpuRunLockWait);
    static constexpr auto last_kind =
        static_cast<std::size_t>(PerfLatencyKind::CpuRunTotal);
    static constexpr auto phase_count = last_kind - first_kind + 1U;

    CpuRunPhaseDiagnostics(
        std::uint32_t process_id, std::uint32_t processor_id,
        std::uint32_t execution_slot, std::uint64_t requested_ticks)
        : process_id_{process_id},
          processor_id_{processor_id},
          execution_slot_{execution_slot},
          requested_ticks_{requested_ticks},
          enabled_{performance_counters().cpu_source_diagnostics_enabled()} {
        if (enabled_) {
            total_started_ = std::chrono::steady_clock::now();
            phase_started_ = total_started_;
        }
    }

    CpuRunPhaseDiagnostics(const CpuRunPhaseDiagnostics&) = delete;
    CpuRunPhaseDiagnostics& operator=(const CpuRunPhaseDiagnostics&) = delete;

    ~CpuRunPhaseDiagnostics() {
        if (!enabled_) return;
        const auto ended = std::chrono::steady_clock::now();
        const auto elapsed = ended - total_started_;
        phase_nanoseconds_.back() = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                .count());
        performance_counters().record_cpu_run_phases(
            process_id_, processor_id_, execution_slot_, total_started_, ended,
            phase_nanoseconds_, requested_ticks_, consumed_ticks_,
            host_yield_checks_, host_yielded_, svc_calls_, svc_);
    }

    void checkpoint(PerfLatencyKind kind) {
        if (!enabled_) return;
        const auto ended = std::chrono::steady_clock::now();
        const auto index = static_cast<std::size_t>(kind) - first_kind;
        if (index < phase_nanoseconds_.size() - 1U) {
            phase_nanoseconds_[index] = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    ended - phase_started_)
                    .count());
        }
        phase_started_ = ended;
    }

    void record_result(
        std::uint64_t consumed_ticks, std::uint64_t host_yield_checks,
        bool host_yielded, std::uint64_t svc_calls,
        std::optional<std::uint32_t> svc) noexcept {
        consumed_ticks_ = consumed_ticks;
        host_yield_checks_ = host_yield_checks;
        host_yielded_ = host_yielded;
        svc_calls_ = svc_calls;
        svc_ = svc;
    }

private:
    std::uint32_t process_id_{};
    std::uint32_t processor_id_{};
    std::uint32_t execution_slot_{};
    std::uint64_t requested_ticks_{};
    std::uint64_t consumed_ticks_{};
    std::uint64_t host_yield_checks_{};
    bool host_yielded_{};
    std::uint64_t svc_calls_{};
    std::optional<std::uint32_t> svc_;
    bool enabled_{};
    std::chrono::steady_clock::time_point total_started_;
    std::chrono::steady_clock::time_point phase_started_;
    std::array<std::uint64_t, phase_count> phase_nanoseconds_{};
};

[[nodiscard]] std::uint64_t logical_committed_code_bytes(
    std::uint64_t used_bytes) noexcept {
    // Linux Dynarmic maps the complete slab in one anonymous mapping and does
    // not expose a page-commit counter.  Count the emitted code range rounded
    // to host pages as logical committed code; physical residency remains the
    // separately sampled process RSS.
    if (used_bytes == 0U) return 0U;
    const auto remainder = used_bytes % host_code_page_size;
    return used_bytes +
           (remainder == 0U ? 0U : host_code_page_size - remainder);
}

constexpr std::uint32_t jit_artifact_hle_abi_version = 1U;
constexpr std::uint32_t jit_artifact_backend_abi_version = 2U;
constexpr std::uint64_t jit_artifact_codegen_options = 1U;
// Layout identity no longer contains host vnode metadata. Keep old records
// safely unusable even if a caller happens to reconstruct the same key shape.
constexpr std::uint32_t jit_artifact_format_version = 8U;

#ifndef ILEMU_DYNARMIC_BUILD_FINGERPRINT
#define ILEMU_DYNARMIC_BUILD_FINGERPRINT 0x0ULL
#endif

constexpr std::uint64_t jit_artifact_dynarmic_build_fingerprint =
    ILEMU_DYNARMIC_BUILD_FINGERPRINT;
// A zero fingerprint means the dependency producer could not be identified
// (for example, when Dynarmic is supplied without its Git metadata).  Such a
// key cannot establish producer compatibility, so it must never authorize a
// persistent IR import.
constexpr bool jit_artifact_producer_fingerprint_available =
    jit_artifact_dynarmic_build_fingerprint != 0U;

[[nodiscard]] ArmCpuModelKind jit_artifact_cpu_model(
    const ArmCpuModel& cpu_model) noexcept {
    return cpu_model.architecture_version() == ArmArchitectureVersion::Armv7
               ? ArmCpuModelKind::CortexA8
               : ArmCpuModelKind::Arm1176JzfS;
}

[[nodiscard]] JitHostIsa jit_artifact_host_isa() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
    return JitHostIsa::Arm64;
#elif defined(__x86_64__) || defined(_M_X64)
    return JitHostIsa::X86_64;
#else
    return JitHostIsa::Unknown;
#endif
}

[[nodiscard]] std::uint64_t jit_artifact_host_feature_mask() noexcept {
    static const auto mask = [] {
        std::uint64_t result = 0;
#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(_M_X64))
        // Keep these bit positions aligned with Dynarmic's X64 HostFeature
        // enum. Its emitter selects different instructions for these
        // capabilities, so the portable-IR key must distinguish them even
        // though the artifact itself is not native host code.
        __builtin_cpu_init();
        if (__builtin_cpu_supports("ssse3")) result |= std::uint64_t{1} << 0U;
        if (__builtin_cpu_supports("sse4.1")) result |= std::uint64_t{1} << 1U;
        if (__builtin_cpu_supports("sse4.2")) result |= std::uint64_t{1} << 2U;
        if (__builtin_cpu_supports("avx")) result |= std::uint64_t{1} << 3U;
        if (__builtin_cpu_supports("avx2")) result |= std::uint64_t{1} << 4U;
        if (__builtin_cpu_supports("avx512f")) result |= std::uint64_t{1} << 5U;
        if (__builtin_cpu_supports("avx512cd")) result |= std::uint64_t{1} << 6U;
        if (__builtin_cpu_supports("avx512vl")) result |= std::uint64_t{1} << 7U;
        if (__builtin_cpu_supports("avx512bw")) result |= std::uint64_t{1} << 8U;
        if (__builtin_cpu_supports("avx512dq")) result |= std::uint64_t{1} << 9U;
        if (__builtin_cpu_supports("avx512bitalg")) result |= std::uint64_t{1} << 10U;
        if (__builtin_cpu_supports("avx512vbmi")) result |= std::uint64_t{1} << 11U;
        if (__builtin_cpu_supports("pclmul")) result |= std::uint64_t{1} << 12U;
        if (__builtin_cpu_supports("f16c")) result |= std::uint64_t{1} << 13U;
        if (__builtin_cpu_supports("fma")) result |= std::uint64_t{1} << 14U;
        if (__builtin_cpu_supports("aes")) result |= std::uint64_t{1} << 15U;
        if (__builtin_cpu_supports("sha")) result |= std::uint64_t{1} << 16U;
        if (__builtin_cpu_supports("popcnt")) result |= std::uint64_t{1} << 17U;
        if (__builtin_cpu_supports("bmi")) result |= std::uint64_t{1} << 18U;
        if (__builtin_cpu_supports("bmi2")) result |= std::uint64_t{1} << 19U;
        if (__builtin_cpu_supports("lzcnt")) result |= std::uint64_t{1} << 20U;
        if (__builtin_cpu_supports("gfni")) result |= std::uint64_t{1} << 21U;
#endif
        return result;
    }();
    return mask;
}

[[nodiscard]] constexpr bool portable_artifact_import_supported() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return true;
#else
    // Dynarmic's ARM64 A32 emitter currently rejects Interpret terminals;
    // keep portable IR as a publish-only diagnostic/cache format until that
    // backend can validate and emit every imported terminal safely.
    return false;
#endif
}

}  // namespace

class JitCallbacks final : public Dynarmic::A32::UserCallbacks {
public:
    enum class ArtifactImportOutcome : std::uint8_t {
        Unavailable,
        Imported,
        AlreadyPresent,
        Failed,
    };

    JitCallbacks(
        AddressSpace& memory,
        const ArmCpuModel& cpu_model,
        std::shared_ptr<JitArtifactStore> artifact_store,
        std::shared_ptr<JitNativePreimportTracker> native_preimport_tracker)
        : memory_{memory},
          cpu_model_{cpu_model},
          artifact_store_{std::move(artifact_store)},
          native_preimport_tracker_{std::move(native_preimport_tracker)} {}

    void attach(Cpu* owner, Dynarmic::A32::Jit* jit) {
        owner_ = owner;
        jit_ = jit;
    }

    bool PreCodeReadHook(
    bool, Dynarmic::A32::VAddr address,
    Dynarmic::A32::IREmitter& ir) override {
        if (ir.block.CycleCount() == 0) {
            performance_counters().record_translation_block();
            translation_block_ = &ir.block;
            translation_code_pages_.clear();
            translation_constant_dependencies_.clear();
            constant_dependency_failed_ = false;
        }
        if (translation_block_ == &ir.block &&
            (portable_generation_location_ || explicit_artifact_publication_)) {
            const auto page = address & ~(AddressSpace::page_size - 1U);
            if (std::find(translation_code_pages_.begin(),
                          translation_code_pages_.end(), page) ==
                translation_code_pages_.end()) {
                translation_code_pages_.push_back(page);
            }
        }
        // This fork's translator continues normal decoding when the hook returns
        // true. Returning false is reserved for a hook that already emitted an IR
        // terminal. (The comment in UserCallbacks currently says the opposite.)
        return true;
    }

    void CodeTranslationCompleted(
        std::uint64_t location_descriptor,
        std::uint64_t translation_nanoseconds) noexcept override {
        translation_completed(location_descriptor, translation_nanoseconds,
                              nullptr);
    }

    void CodeTranslationCompleted(
        std::uint64_t location_descriptor,
        std::uint64_t translation_nanoseconds,
        const Dynarmic::IR::Block& block) noexcept override {
        translation_completed(location_descriptor, translation_nanoseconds,
                              &block);
    }

    [[nodiscard]] ArtifactImportOutcome import_artifact(
        Dynarmic::A32::Jit& jit,
        std::uint64_t location_descriptor) const noexcept {
        auto block = validated_artifact_block(location_descriptor);
        if (!block) return ArtifactImportOutcome::Unavailable;
        try {
            return jit.Precompile(std::move(*block))
                       ? ArtifactImportOutcome::Imported
                       : ArtifactImportOutcome::AlreadyPresent;
        } catch (...) {
            return ArtifactImportOutcome::Failed;
        }
    }

    [[nodiscard]] bool artifact_available(
        std::uint64_t location_descriptor) const noexcept {
        return validated_artifact_block(location_descriptor).has_value();
    }

    [[nodiscard]] bool generate_portable_artifact(
        Dynarmic::A32::Jit& jit,
        std::uint64_t location_descriptor) noexcept {
        if (!artifact_store_ ||
            !jit_artifact_producer_fingerprint_available) {
            return false;
        }
        try {
            portable_generation_location_ = location_descriptor;
            portable_generation_published_ = false;
            jit.GeneratePortableIR(location_descriptor);
            portable_generation_location_.reset();
            return portable_generation_published_;
        } catch (...) {
            portable_generation_location_.reset();
            portable_generation_published_ = false;
            return false;
        }
    }

    void set_explicit_artifact_publication(bool enabled) noexcept {
        explicit_artifact_publication_ = enabled;
    }

    [[nodiscard]] std::shared_ptr<const BlockArtifact> find_artifact(
        std::uint64_t location_descriptor) const noexcept {
        if (!artifact_store_) return nullptr;
        const auto key = make_artifact_key(location_descriptor);
        return key ? artifact_store_->find(*key, artifact_retention_)
                   : nullptr;
    }

    [[nodiscard]] std::optional<JitArtifactKey> artifact_key(
        std::uint64_t location_descriptor) const noexcept {
        return make_artifact_key(location_descriptor);
    }

    [[nodiscard]] std::uint64_t artifact_publication_generation() const
        noexcept {
        return artifact_store_ ? artifact_store_->publication_generation() : 0U;
    }

    // Preparation is deliberately separate from the Dynarmic miss callback:
    // store lookup, dependency validation, and IR deserialization all happen
    // before Jit::Run. The miss callback only consumes this executor-local
    // slot after NativeCodeSlab::find_block has failed.
    bool stage_demand_artifact(
        std::uint64_t location_descriptor,
        std::uint64_t slab_generation) {
        try {
            if (demand_artifact_location_ == location_descriptor &&
                demand_artifact_slab_generation_ == slab_generation &&
                demand_artifact_ && !demand_artifact_consumed_) {
                return true;
            }
            discard_demand_artifact();

            const auto key = make_artifact_key(location_descriptor);
            if (!key) return false;
            auto block = validated_artifact_block(location_descriptor);
            if (!block || block->Location().Value() != location_descriptor) {
                return false;
            }
            demand_artifact_key_ = *key;
            demand_artifact_location_ = location_descriptor;
            demand_artifact_slab_generation_ = slab_generation;
            demand_artifact_.emplace(std::move(*block));
            if (translation_profile_) {
                translation_profile_->note_demand_artifact_staged();
            }
            return true;
        } catch (...) {
            discard_demand_artifact();
            return false;
        }
    }

    [[nodiscard]] bool demand_artifact_staged(
        std::uint64_t location_descriptor,
        std::uint64_t slab_generation) const noexcept {
        return demand_artifact_location_ == location_descriptor &&
               demand_artifact_slab_generation_ == slab_generation &&
               demand_artifact_.has_value() &&
               !demand_artifact_consumed_;
    }

    [[nodiscard]] bool demand_artifact_native_ready(
        std::uint64_t location_descriptor,
        std::uint64_t slab_generation) const noexcept {
        return demand_artifact_location_ == location_descriptor &&
               demand_artifact_slab_generation_ == slab_generation &&
               demand_artifact_consumed_;
    }

    void discard_demand_artifact() noexcept {
        if (demand_artifact_ && !demand_artifact_consumed_ &&
            translation_profile_) {
            translation_profile_->note_demand_artifact_stage_unused();
        }
        demand_artifact_.reset();
        demand_artifact_key_ = {};
        demand_artifact_location_ = 0;
        demand_artifact_slab_generation_ = 0;
        demand_artifact_consumed_ = false;
    }

    void finish_demand_artifact(std::uint64_t location_descriptor) noexcept {
        if (demand_artifact_ && !demand_artifact_consumed_ &&
            demand_artifact_location_ == location_descriptor) {
            discard_demand_artifact();
        }
    }

    // Called by Dynarmic only after NativeCodeSlab::find_block misses. This
    // function does not access the store and does not allocate or lock.
    [[nodiscard]] Dynarmic::IR::Block* take_demand_artifact(
        std::uint64_t location_descriptor,
        std::uint64_t slab_generation) noexcept {
        const bool hit = demand_artifact_ && !demand_artifact_consumed_ &&
                         demand_artifact_location_ == location_descriptor &&
                         demand_artifact_slab_generation_ == slab_generation &&
                         demand_artifact_key_.location_descriptor ==
                             location_descriptor &&
                         demand_artifact_->Location().Value() ==
                             location_descriptor;
        performance_counters().record_jit_demand_artifact_probe(hit);
        if (!hit) return nullptr;
        demand_artifact_consumed_ = true;
        if (translation_profile_) {
            translation_profile_->note_demand_artifact_consumed();
        }
        return &*demand_artifact_;
    }

    void discard_translation_location(
        std::uint64_t location_descriptor) noexcept {
        if (translation_profile_) {
            translation_profile_->discard(location_descriptor);
        }
    }

private:
    [[nodiscard]] std::optional<Dynarmic::IR::Block>
    validated_artifact_block(
        std::uint64_t location_descriptor) const noexcept {
        if (!artifact_store_ || !portable_artifact_import_supported() ||
            !jit_artifact_producer_fingerprint_available) {
            if (artifact_store_) {
                artifact_store_->record_validation_rejection(
                    JitArtifactValidationRejection::Unavailable);
            }
            return std::nullopt;
        }
        try {
            const auto artifact = find_artifact(location_descriptor);
            if (!artifact) {
                artifact_store_->record_validation_rejection(
                    JitArtifactValidationRejection::NoExactArtifact);
                return std::nullopt;
            }
            if (artifact->data.normalized_ir.empty()) {
                artifact_store_->record_validation_rejection(
                    JitArtifactValidationRejection::EmptyIr);
                return std::nullopt;
            }
            if (!dependencies_match(*artifact)) {
                artifact_store_->record_validation_rejection(
                    JitArtifactValidationRejection::DependencyMismatch);
                return std::nullopt;
            }
            auto block = deserialize_dynarmic_ir(artifact->data.normalized_ir);
            if (!block) {
                artifact_store_->record_validation_rejection(
                    JitArtifactValidationRejection::DeserializeFailed);
                return std::nullopt;
            }
            if (block->Location().Value() != location_descriptor) {
                artifact_store_->record_validation_rejection(
                    JitArtifactValidationRejection::DescriptorMismatch);
                return std::nullopt;
            }
            return block;
        } catch (...) {
            artifact_store_->record_validation_rejection(
                JitArtifactValidationRejection::Exception);
            return std::nullopt;
        }
    }

    void record_constant_dependency(
        std::uint32_t address, std::uint32_t size,
        std::uint64_t value) {
        if ((!portable_generation_location_ &&
             !explicit_artifact_publication_) ||
            translation_block_ == nullptr ||
            constant_dependency_failed_) {
            return;
        }
        const auto identity =
            memory_.executable_backing_identity(address, size);
        if (!identity) {
            constant_dependency_failed_ = true;
            return;
        }
        for (const auto &existing : translation_constant_dependencies_) {
            if (existing.address == address && existing.size == size) {
                if (existing.value != value ||
                    existing.content_identity != identity->content ||
                    existing.layout_identity != identity->layout) {
                    constant_dependency_failed_ = true;
                }
                return;
            }
        }
        translation_constant_dependencies_.push_back(JitConstantDependency{
            address, size, value, identity->content, identity->layout});
    }

    [[nodiscard]] bool dependencies_match(
        const BlockArtifact &artifact) const noexcept {
        if (artifact.data.code_dependencies.empty()) return false;
        for (const auto &dependency : artifact.data.code_dependencies) {
            if (dependency.size == 0 ||
                !memory_.is_read_only_executable(dependency.address,
                                                 dependency.size)) {
                return false;
            }
            const auto current = memory_.executable_backing_identity(
                dependency.address, dependency.size);
            if (!current || current->content != dependency.content_identity ||
                current->layout != dependency.layout_identity) {
                return false;
            }
        }
        for (const auto &constant : artifact.data.constant_dependencies) {
            const auto current = memory_.executable_backing_identity(
                constant.address, constant.size);
            if (!current || current->content != constant.content_identity ||
                current->layout != constant.layout_identity) {
                return false;
            }
            std::optional<std::uint64_t> value;
            switch (constant.size) {
            case 1U: {
                const auto read = memory_.read8(
                    constant.address, MemoryPermission::Read);
                if (read) value = *read;
                break;
            }
            case 2U: {
                const auto read = memory_.read16(
                    constant.address, MemoryPermission::Read);
                if (read) value = *read;
                break;
            }
            case 4U: {
                const auto read = memory_.read32(
                    constant.address, MemoryPermission::Read);
                if (read) value = *read;
                break;
            }
            case 8U: {
                const auto read = memory_.read64(
                    constant.address, MemoryPermission::Read);
                if (read) value = *read;
                break;
            }
            default:
                return false;
            }
            if (!value || *value != constant.value) return false;
        }
        return true;
    }

    void translation_completed(
        std::uint64_t location_descriptor,
        std::uint64_t translation_nanoseconds,
        const Dynarmic::IR::Block* optimized_block) noexcept {
        maybe_check_host_yield(
            0U, translation_nanoseconds >=
                       static_cast<std::uint64_t>(
                           std::chrono::duration_cast<std::chrono::nanoseconds>(
                               host_yield_slow_translation_threshold)
                               .count()));
        // Ordinary guest execution is latency-sensitive. Artifact production
        // remains reserved for an explicit precompile request, but a complete
        // descriptor is retained in fixed executor-local storage for a later
        // safe-point profile merge. No store lookup, IR serialization, or
        // profile lock is allowed on this callback path.
        if (!portable_generation_location_ &&
            !explicit_artifact_publication_) {
            static_cast<void>(translation_recorder_.record(location_descriptor));
            performance_counters().record_latency(
                PerfLatencyKind::JitDemandTranslation,
                translation_nanoseconds);
            return;
        }
        const auto published = publish_artifact(
            location_descriptor, translation_nanoseconds, optimized_block);
        if (portable_generation_location_ == location_descriptor) {
            portable_generation_published_ = published;
        }
    }

public:

    void raise_memory_fault(std::uint32_t address, std::size_t size,
                            MemoryPermission access) {
        memory_fault(address, size, access);
    }

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t address) override {
        const auto value = memory_.read32(address, MemoryPermission::Execute);
        if (!value) {
            memory_fault(address, 4, MemoryPermission::Execute);
        }
        return value;
    }

    std::uint8_t MemoryRead8(std::uint32_t address) override {
        const auto value = memory_.read8(address, MemoryPermission::Read);
        if (!value) {
            memory_fault(address, sizeof(std::uint8_t),
                         MemoryPermission::Read);
            return 0;
        }
        record_constant_dependency(address, 1U, *value);
        return *value;
    }
    std::uint16_t MemoryRead16(std::uint32_t address) override {
        const auto value = memory_.read16(address, MemoryPermission::Read);
        if (!value) {
            memory_fault(address, sizeof(std::uint16_t),
                         MemoryPermission::Read);
            return 0;
        }
        record_constant_dependency(address, 2U, *value);
        return *value;
    }
    std::uint32_t MemoryRead32(std::uint32_t address) override {
        const auto value = memory_.read32(address, MemoryPermission::Read);
        if (!value) {
            memory_fault(address, sizeof(std::uint32_t),
                         MemoryPermission::Read);
            return 0;
        }
        record_constant_dependency(address, 4U, *value);
        return *value;
    }
    std::uint64_t MemoryRead64(std::uint32_t address) override {
        const auto value = memory_.read64(address, MemoryPermission::Read);
        if (!value) {
            memory_fault(address, sizeof(std::uint64_t),
                         MemoryPermission::Read);
            return 0;
        }
        record_constant_dependency(address, 8U, *value);
        return *value;
    }

    void MemoryWrite8(std::uint32_t address, std::uint8_t value) override {
        write(address, value, &AddressSpace::write8);
    }
    void MemoryWrite16(std::uint32_t address, std::uint16_t value) override {
        write(address, value, &AddressSpace::write16);
    }
    void MemoryWrite32(std::uint32_t address, std::uint32_t value) override {
        write(address, value, &AddressSpace::write32);
    }
    void MemoryWrite64(std::uint32_t address, std::uint64_t value) override {
        write(address, value, &AddressSpace::write64);
    }

    void MemoryReadExclusive(std::uint32_t address,
                             std::size_t size) override {
        memory_.track_exclusive_access(address, size);
    }

    std::uint8_t MemorySwap8(
        std::uint32_t address, std::uint8_t value) override {
        return swap(address, value, &AddressSpace::exchange8);
    }
    std::uint32_t MemorySwap32(
        std::uint32_t address, std::uint32_t value) override {
        return swap(address, value, &AddressSpace::exchange32);
    }

    bool MemoryWriteExclusive8(
        std::uint32_t address, std::uint8_t value, std::uint8_t expected) override {
        const auto written = memory_.compare_exchange8(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }
    bool MemoryWriteExclusive16(
        std::uint32_t address, std::uint16_t value, std::uint16_t expected) override {
        const auto written = memory_.compare_exchange16(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }
    bool MemoryWriteExclusive32(
        std::uint32_t address, std::uint32_t value, std::uint32_t expected) override {
        const auto written = memory_.compare_exchange32(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }
    bool MemoryWriteExclusive64(
        std::uint32_t address, std::uint64_t value, std::uint64_t expected) override {
        const auto written = memory_.compare_exchange64(address, expected, value);
        if (written) notify_memory_write(address, sizeof(value), value);
        return written;
    }

    bool IsReadOnlyMemory(std::uint32_t address) override {
        return memory_.is_read_only_executable(address, sizeof(std::uint32_t));
    }

    void InterpreterFallback(std::uint32_t pc, std::size_t count) override {
        std::ostringstream message;
        message << "Dynarmic interpreter fallback at 0x" << std::hex << pc
                << " for " << std::dec << count << " instruction(s)";
        exception_ = message.str();
        jit_->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    void CallSVC(std::uint32_t immediate) override {
        performance_counters().record_svc();
        ++svc_calls_;
        svc_ = immediate;
        if (owner_->svc_dispatch_mode_ == SvcDispatchMode::Deferred) {
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (owner_->svc_handler_) {
            owner_->svc_handler_(*owner_, immediate);
        } else {
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
    }

    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override {
        if (exception == Dynarmic::A32::Exception::Breakpoint &&
            owner_->debug_breakpoints_enabled_) {
            breakpoint_ = pc;
            owner_->registers()[15] = pc;
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined7);
            return;
        }
        std::ostringstream message;
        message << "ARM exception " << static_cast<unsigned>(exception)
                << " at 0x" << std::hex << pc;
        exception_ = message.str();
        jit_->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    void AddTicks(std::uint64_t ticks) override {
        consumed_ += ticks;
        ticks_remaining_ = ticks >= ticks_remaining_ ? 0 : ticks_remaining_ - ticks;
        if (!cooperative_execution_ || host_yield_requested_) {
            return;
        }
        maybe_check_host_yield(ticks, ticks_remaining_ == 0U);
    }
    std::uint64_t GetTicksRemaining() override { return ticks_remaining_; }
    std::uint64_t GetTicksForCode(
        bool is_thumb, Dynarmic::A32::VAddr address,
        std::uint32_t instruction) override {
        return cpu_model_.ticks_for_instruction(
            is_thumb, address, instruction);
    }

    void begin(
        std::uint64_t ticks, bool cooperative_execution = false,
        std::chrono::nanoseconds host_slice_budget =
            default_host_cooperative_slice_budget) {
        ticks_remaining_ = ticks;
        consumed_ = 0;
        svc_.reset();
        svc_calls_ = 0;
        fault_.reset();
        breakpoint_.reset();
        exception_.clear();
        cooperative_execution_ = cooperative_execution && ticks != 0U;
        host_yield_requested_ = false;
        host_yield_probe_count_ = 0;
        host_yield_tick_accumulator_ = 0;
        host_yield_checks_ = 0;
        host_yield_check_interval_ = host_yield_initial_check_interval;
        host_yield_tick_budget_ = host_yield_initial_tick_budget;
        host_slice_deadline_ = cooperative_execution_
            ? std::chrono::steady_clock::now() +
                  std::max(host_slice_budget, std::chrono::nanoseconds::zero())
            : std::chrono::steady_clock::time_point{};
    }

    CpuRunResult result(Dynarmic::HaltReason reason) const {
        return CpuRunResult{
            reason, consumed_, svc_, svc_calls_, fault_, breakpoint_,
            exception_, host_yield_requested_, host_yield_checks_};
    }

    [[nodiscard]] const ArmCpuModel& cpu_model() const {
        return cpu_model_;
    }
    [[nodiscard]] Cpu* current_cpu() const { return owner_; }
    [[nodiscard]] std::uint8_t** jit_read_page_table() {
        return memory_.jit_read_page_table();
    }
    [[nodiscard]] std::uint8_t** jit_write_page_table() {
        return memory_.jit_write_page_table();
    }
    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile) {
        flush_translation_profile_recorder();
        translation_profile_ = std::move(profile);
    }

    // Drain only after Dynarmic has returned to a host safe point. The hot
    // callback records raw descriptors into fixed executor-local storage; all
    // address-space checks, profile locking, and metric aggregation happen
    // here instead of in CodeTranslationCompleted.
    void flush_translation_profile_recorder() noexcept {
        const auto recorded = translation_recorder_.locations();
        if (recorded.empty() && translation_recorder_.deduplicated() == 0U &&
            translation_recorder_.dropped_capacity() == 0U) {
            return;
        }
        std::array<std::uint64_t,
                   jit_translation_profile_recorder_capacity>
            stable_locations{};
        std::size_t stable_count{};
        std::uint64_t unstable_count{};
        for (const auto location_descriptor : recorded) {
            native_preimport_tracker_->mark_demand_seen(location_descriptor);
            const auto code_address =
                static_cast<std::uint32_t>(location_descriptor) &
                ~std::uint32_t{3};
            bool stable = false;
            try {
                stable = memory_.translation_profile_stable(
                             code_address, sizeof(std::uint32_t)) &&
                         memory_.is_read_only_executable(
                             code_address, sizeof(std::uint32_t));
            } catch (...) {
                stable = false;
            }
            if (stable && stable_count < stable_locations.size()) {
                stable_locations[stable_count++] = location_descriptor;
            } else {
                ++unstable_count;
            }
        }
        if (translation_profile_) {
            translation_profile_->merge(
                std::span<const std::uint64_t>{stable_locations.data(),
                                                stable_count},
                translation_recorder_.deduplicated(),
                translation_recorder_.dropped_capacity() + unstable_count);
            translation_profile_->note_unstable_dropped(unstable_count);
        }
        translation_recorder_.reset();
    }

    [[nodiscard]] bool demand_location_seen(
        std::uint64_t location_descriptor) const noexcept {
        return native_preimport_tracker_->demand_seen(location_descriptor);
    }

    void clear_demand_locations() noexcept {
        native_preimport_tracker_->clear_demand_locations();
    }

    void note_profile_portable_existence_hit() noexcept {
        if (translation_profile_) {
            translation_profile_->note_portable_existence_hit();
        }
    }
    void note_profile_portable_generated() noexcept {
        if (translation_profile_) {
            translation_profile_->note_profile_portable_generated();
        }
    }
    void note_native_preimport_attempted() noexcept {
        if (translation_profile_) {
            translation_profile_->note_native_preimport_attempted();
        }
    }
    void note_native_preimport_imported() noexcept {
        if (translation_profile_) {
            translation_profile_->note_native_preimport_imported();
        }
    }
    void note_native_preimport_already_present() noexcept {
        if (translation_profile_) {
            translation_profile_->note_native_preimport_already_present();
        }
    }
    void note_native_preimport_before_first_demand() noexcept {
        if (translation_profile_) {
            translation_profile_->note_native_preimport_before_first_demand();
        }
    }
    void note_profile_imported_before_first_run() noexcept {
        if (translation_profile_) {
            translation_profile_->note_profile_imported_before_first_run();
        }
    }
    void note_native_preimport_used() noexcept {
        if (translation_profile_) {
            translation_profile_->note_native_preimport_used();
        }
    }

    void set_artifact_retention(JitArtifactRetention retention) noexcept {
        artifact_retention_ = retention;
    }

private:
    void maybe_check_host_yield(
        std::uint64_t ticks, bool force_clock_check) noexcept {
        if (!cooperative_execution_ || host_yield_requested_ ||
            (!force_clock_check && ticks_remaining_ == 0U)) {
            return;
        }
        host_yield_tick_accumulator_ =
            std::min(host_yield_max_tick_budget,
                     host_yield_tick_accumulator_ + ticks);
        const auto probe_count = ++host_yield_probe_count_;
        if (!force_clock_check &&
            probe_count < host_yield_check_interval_ &&
            host_yield_tick_accumulator_ < host_yield_tick_budget_) {
            return;
        }
        host_yield_probe_count_ = 0;
        host_yield_tick_accumulator_ = 0;
        ++host_yield_checks_;
        const auto now = std::chrono::steady_clock::now();
        if (now >= host_slice_deadline_) {
            request_host_yield();
            return;
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                host_slice_deadline_ - now);
        if (remaining <= host_yield_urgent_window) {
            host_yield_check_interval_ = host_yield_min_check_interval;
            host_yield_tick_budget_ = host_yield_min_tick_budget;
        } else if (remaining <= std::chrono::milliseconds{1}) {
            host_yield_check_interval_ =
                (host_yield_min_check_interval +
                 host_yield_initial_check_interval) /
                2U;
            host_yield_tick_budget_ = host_yield_initial_tick_budget / 2U;
        } else {
            host_yield_check_interval_ = std::min(
                host_yield_max_check_interval,
                host_yield_check_interval_ + host_yield_min_check_interval);
            host_yield_tick_budget_ = std::min(
                host_yield_max_tick_budget,
                host_yield_tick_budget_ * 2U);
        }
    }

    [[nodiscard]] std::optional<JitArtifactKey> make_artifact_key(
        std::uint64_t location_descriptor) const noexcept {
        try {
            const auto pc = static_cast<std::uint32_t>(location_descriptor);
            const auto backing = memory_.executable_backing_identity(
                pc & ~std::uint32_t{3}, sizeof(std::uint32_t));
            if (!backing) return std::nullopt;

            JitArtifactKey key;
            key.content_identity = backing->content;
            key.layout_identity = backing->layout;
            key.guest_pc = pc;
            key.thumb = ((location_descriptor >> 32U) & 1U) != 0;
            key.location_descriptor = location_descriptor;
            key.architecture = cpu_model_.architecture_version();
            key.cpu_model = jit_artifact_cpu_model(cpu_model_);
            key.timing_model_version = 1U;
            key.guest_ticks_per_second = cpu_model_.ticks_per_second();
            // The effective Guest mapping is already part of layout_identity;
            // no Mach-O slide is available at this generic CPU boundary.
            key.image_slide = 0U;
            key.hle_abi_version = jit_artifact_hle_abi_version;
            key.backend_abi_version = jit_artifact_backend_abi_version;
            key.dynarmic_build_fingerprint =
                jit_artifact_dynarmic_build_fingerprint;
            key.codegen_options = jit_artifact_codegen_options;
            key.host_isa = jit_artifact_host_isa();
            key.host_feature_mask = jit_artifact_host_feature_mask();
            key.artifact_format_version = jit_artifact_format_version;
            return key;
        } catch (...) {
            return std::nullopt;
        }
    }

    [[nodiscard]] bool publish_artifact(
        std::uint64_t location_descriptor,
        std::uint64_t translation_nanoseconds,
        const Dynarmic::IR::Block* optimized_block) noexcept {
        if (!artifact_store_) return false;
        try {
            const auto *translation_block = translation_block_;
            translation_block_ = nullptr;
            auto key = make_artifact_key(location_descriptor);
            if (!key) return false;
            if (translation_code_pages_.empty()) return false;
            if (constant_dependency_failed_) return false;

            JitArtifactData data;
            data.code_dependencies.reserve(translation_code_pages_.size());
            for (const auto page : translation_code_pages_) {
                const auto dependency =
                    memory_.executable_backing_identity(
                        page, AddressSpace::page_size);
                if (!dependency) return false;
                data.code_dependencies.push_back(JitCodeDependency{
                    page, AddressSpace::page_size, dependency->content,
                    dependency->layout});
            }
            data.constant_dependencies = translation_constant_dependencies_;
            if (optimized_block != nullptr) {
                const auto serialized = serialize_dynarmic_ir(*optimized_block);
                if (!serialized) return false;
                if (portable_generation_location_ == location_descriptor) {
                    const auto validated = deserialize_dynarmic_ir(*serialized);
                    if (!validated ||
                        validated->Location().Value() != location_descriptor) {
                        return false;
                    }
                }
                data.normalized_ir = *serialized;
            } else if (translation_block != nullptr) {
                // Retain a readable fallback for legacy callback users, but
                // it is intentionally not importable as portable IR.
                data.normalized_ir = normalized_ir(*translation_block);
            }
            data.translation_nanoseconds = translation_nanoseconds;
            return artifact_store_->publish(
                       std::move(*key), std::move(data), artifact_retention_) !=
                   nullptr;
        } catch (...) {
            // Artifact persistence must never make guest execution fail.
            return false;
        }
    }

    [[nodiscard]] static std::vector<std::byte> normalized_ir(
        const Dynarmic::IR::Block &block) {
        auto dump = Dynarmic::IR::DumpBlock(block);
        std::string canonical;
        canonical.reserve(dump.size());
        for (std::size_t index = 0; index < dump.size();) {
            if (dump[index] == '[' && index + 17U < dump.size() &&
                dump[index + 17U] == ']' &&
                std::all_of(dump.begin() + static_cast<std::ptrdiff_t>(index + 1U),
                            dump.begin() + static_cast<std::ptrdiff_t>(index + 17U),
                            [](unsigned char value) {
                                return std::isxdigit(value) != 0;
                            })) {
                canonical += "[inst]";
                index += 18U;
                continue;
            }
            constexpr std::string_view unnamed = "<unnamed inst ";
            if (dump.compare(index, unnamed.size(), unnamed) == 0) {
                canonical += "<unnamed inst>";
                const auto end = dump.find('>', index + unnamed.size());
                index = end == std::string::npos ? dump.size() : end + 1U;
                continue;
            }
            canonical.push_back(dump[index++]);
        }
        const auto *begin =
            reinterpret_cast<const std::byte *>(canonical.data());
        return std::vector<std::byte>(begin, begin + canonical.size());
    }

    template<typename T, typename Member>
    T read(std::uint32_t address, Member member) {
        const auto value = (memory_.*member)(address, MemoryPermission::Read);
        if (!value) {
            memory_fault(address, sizeof(T), MemoryPermission::Read);
            return 0;
        }
        return *value;
    }

    template<typename T, typename Member>
    void write(std::uint32_t address, T value, Member member) {
        if (!(memory_.*member)(address, value)) {
            memory_fault(address, sizeof(T), MemoryPermission::Write);
        } else {
            notify_memory_write(address, sizeof(T), value);
        }
    }

    template<typename T, typename Member>
    T swap(std::uint32_t address, T value, Member member) {
        const auto previous = (memory_.*member)(address, value);
        if (!previous) {
            memory_fault(address, sizeof(T),
                         MemoryPermission::Read | MemoryPermission::Write);
            return 0;
        }
        notify_memory_write(address, sizeof(T), value);
        return *previous;
    }

    void notify_memory_write(
        std::uint32_t address, std::size_t size, std::uint64_t value) {
        if (!owner_->memory_write_watch_address_ ||
            !owner_->memory_write_handler_) {
            return;
        }
        const auto write_begin = static_cast<std::uint64_t>(address);
        const auto write_end = write_begin + size;
        const auto watched =
            static_cast<std::uint64_t>(*owner_->memory_write_watch_address_);
        if (watched >= write_begin && watched < write_end) {
            owner_->memory_write_handler_(*owner_, address, size, value);
        }
    }

    void memory_fault(std::uint32_t address, std::size_t size, MemoryPermission access) {
        performance_counters().record_page_fault();
        fault_ = MemoryFault{address, size, access, "unmapped address or protection failure"};
        if (jit_ != nullptr) {
            jit_->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
        }
    }

    void request_host_yield() noexcept {
        host_yield_requested_ = true;
        ticks_remaining_ = 0;
        if (jit_ != nullptr) {
            // UserDefined2 is the existing scheduler AST boundary. It keeps
            // the current XNU quantum and therefore does not model a guest
            // yield or alter any kernel-visible ABI state.
            jit_->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
    }

    AddressSpace& memory_;
    const ArmCpuModel& cpu_model_;
    Cpu* owner_{};
    Dynarmic::A32::Jit* jit_{};
    std::uint64_t ticks_remaining_{};
    std::uint64_t consumed_{};
    std::optional<std::uint32_t> svc_;
    std::uint64_t svc_calls_{};
    std::optional<MemoryFault> fault_;
    std::optional<std::uint32_t> breakpoint_;
    std::string exception_;
    std::shared_ptr<JitTranslationProfile> translation_profile_;
    std::shared_ptr<JitArtifactStore> artifact_store_;
    JitArtifactRetention artifact_retention_{JitArtifactRetention::Normal};
    bool explicit_artifact_publication_{};
    std::optional<std::uint64_t> portable_generation_location_;
    bool portable_generation_published_{};
    Dynarmic::IR::Block *translation_block_{};
    std::vector<std::uint32_t> translation_code_pages_;
    std::vector<JitConstantDependency> translation_constant_dependencies_;
    bool constant_dependency_failed_{};
    std::optional<Dynarmic::IR::Block> demand_artifact_;
    JitArtifactKey demand_artifact_key_{};
    std::uint64_t demand_artifact_location_{};
    std::uint64_t demand_artifact_slab_generation_{};
    bool demand_artifact_consumed_{};
    bool cooperative_execution_{};
    bool host_yield_requested_{};
    std::uint32_t host_yield_probe_count_{};
    std::uint32_t host_yield_check_interval_{};
    std::uint64_t host_yield_tick_accumulator_{};
    std::uint64_t host_yield_tick_budget_{};
    std::uint64_t host_yield_checks_{};
    std::chrono::steady_clock::time_point host_slice_deadline_{};
    JitTranslationProfileRecorder translation_recorder_;
    std::shared_ptr<JitNativePreimportTracker> native_preimport_tracker_;
};

// The iPhone ARM user ABI uses CP15 thread-pointer registers in addition to
// the older cthread_self fast trap. Dynarmic deliberately leaves CP15 to its
// client, so model only the architecturally visible user-thread and barrier
// subset here. Memory is coherent in AddressSpace; cache/barrier operations
// therefore need no host-side work, but must remain legal instructions.
class ArmSystemControlCoprocessor final
    : public Dynarmic::A32::Coprocessor {
public:
    using CoprocReg = Dynarmic::A32::CoprocReg;
    using Callback = Dynarmic::A32::Coprocessor::Callback;
    using CallbackOrAccessOneWord =
        Dynarmic::A32::Coprocessor::CallbackOrAccessOneWord;
    using CallbackOrAccessTwoWords =
        Dynarmic::A32::Coprocessor::CallbackOrAccessTwoWords;

    explicit ArmSystemControlCoprocessor(JitCallbacks& callbacks)
        : callbacks_{callbacks} {}

    std::optional<Callback> CompileInternalOperation(
        bool, unsigned, CoprocReg, CoprocReg, CoprocReg, unsigned) override {
        return std::nullopt;
    }

    CallbackOrAccessOneWord CompileSendOneWord(
        bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        if (two || opc1 != 0) {
            return std::monostate{};
        }

        // The guest ARM cache maintenance instructions are no-ops for the
        // coherent host-backed memory model.  Keeping them as callbacks also
        // avoids Dynarmic compiling an illegal-instruction assertion.
        if (CRn == CoprocReg::C7 || CRn == CoprocReg::C8) {
            return Callback{&noop, nullptr};
        }

        // TPIDRURW/TPIDRPRW are the writable per-thread pointers used by the
        // Darwin ARM pthread ABI.  The simulator keeps one logical pointer,
        // shared with the legacy cthread_self fast trap, so old and new
        // firmware observe the same thread context.
        if (CRn == CoprocReg::C13 && CRm == CoprocReg::C0 &&
            (opc2 == 2 || opc2 == 7)) {
            return Callback{&write_thread_pointer, &callbacks_};
        }

        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileSendTwoWords(
        bool, unsigned, CoprocReg) override {
        return std::monostate{};
    }

    CallbackOrAccessOneWord CompileGetOneWord(
        bool two, unsigned opc1, CoprocReg CRn, CoprocReg CRm,
        unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == CoprocReg::C13 &&
            CRm == CoprocReg::C0 && (opc2 == 2 || opc2 == 3 || opc2 == 7)) {
            return Callback{&read_thread_pointer, &callbacks_};
        }
        return std::monostate{};
    }

    CallbackOrAccessTwoWords CompileGetTwoWords(
        bool, unsigned, CoprocReg) override {
        return std::monostate{};
    }

    std::optional<Callback> CompileLoadWords(
        bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }

    std::optional<Callback> CompileStoreWords(
        bool, bool, CoprocReg, std::optional<std::uint8_t>) override {
        return std::nullopt;
    }

private:
    static std::uint64_t noop(void*, std::uint32_t, std::uint32_t) {
        return 0;
    }

    static std::uint64_t read_thread_pointer(
        void* user_arg, std::uint32_t, std::uint32_t) {
        const auto& callbacks =
            *reinterpret_cast<JitCallbacks*>(user_arg);
        const auto* cpu = callbacks.current_cpu();
        return cpu == nullptr ? 0 : cpu->cthread_self().value_or(0);
    }

    static std::uint64_t write_thread_pointer(
        void* user_arg, std::uint32_t value, std::uint32_t) {
        const auto& callbacks =
            *reinterpret_cast<JitCallbacks*>(user_arg);
        if (auto* cpu = callbacks.current_cpu(); cpu != nullptr) {
            cpu->set_cthread_self(value);
        }
        return 0;
    }

    JitCallbacks& callbacks_;
};

class JitExecutor {
public:
    enum class PrecompileDisposition : std::uint8_t {
        NativeCompiled,
        PortableGenerated,
        PortableArtifactHit,
        ArtifactImported,
        ArtifactProbeHit,
        SharedSlabHit,
        Deferred,
        Unstable,
        CacheFull,
        Failed,
    };

    JitExecutor(
        std::size_t processor_id,
        std::size_t execution_slot,
        AddressSpace& memory,
        Dynarmic::ExclusiveMonitor& monitor,
        const ArmCpuModel& cpu_model,
        std::shared_ptr<JitArtifactStore> artifact_store,
        std::shared_ptr<ExecutionContext> execution_context,
        std::shared_ptr<JitNativePreimportTracker> native_preimport_tracker)
        : processor_id_{processor_id},
          execution_slot_{execution_slot},
          memory_{memory},
          monitor_{monitor},
          callbacks_{std::make_unique<JitCallbacks>(
              memory, cpu_model, std::move(artifact_store),
              native_preimport_tracker)},
          cp15_{std::make_unique<ArmSystemControlCoprocessor>(
              *callbacks_)},
          execution_context_{std::move(execution_context)},
          native_preimport_tracker_{std::move(native_preimport_tracker)} {
        if (!execution_context_ || !native_preimport_tracker_) {
            throw std::invalid_argument{
                "JIT executor requires execution state"};
        }
        runtime_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(
            runtime_link_cell_,
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(callbacks_.get())));
        runtime_link_cell_address_ =
            execution_context_->link_cell_address(runtime_link_cell_);
        lookup_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(lookup_link_cell_, 0);
        lookup_link_cell_address_ =
            execution_context_->link_cell_address(lookup_link_cell_);
        runtime_config_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(runtime_config_link_cell_, 0);
        runtime_config_link_cell_address_ =
            execution_context_->link_cell_address(runtime_config_link_cell_);
        fast_dispatch_table_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(fast_dispatch_table_link_cell_, 0);
        fast_dispatch_table_link_cell_address_ =
            execution_context_->link_cell_address(fast_dispatch_table_link_cell_);
        page_table_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(page_table_link_cell_, 0);
        page_table_link_cell_address_ =
            execution_context_->link_cell_address(page_table_link_cell_);
        read_page_table_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(read_page_table_link_cell_, 0);
        read_page_table_link_cell_address_ =
            execution_context_->link_cell_address(read_page_table_link_cell_);
        exclusive_monitor_lock_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(
            exclusive_monitor_lock_link_cell_,
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                Dynarmic::GetExclusiveMonitorLockPointer(&monitor_))));
        exclusive_monitor_lock_link_cell_address_ =
            execution_context_->link_cell_address(exclusive_monitor_lock_link_cell_);
        exclusive_monitor_addresses_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(
            exclusive_monitor_addresses_link_cell_,
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                Dynarmic::GetExclusiveMonitorAddressPointer(&monitor_, 0))));
        exclusive_monitor_addresses_link_cell_address_ =
            execution_context_->link_cell_address(exclusive_monitor_addresses_link_cell_);
        exclusive_monitor_values_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(
            exclusive_monitor_values_link_cell_,
            static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                Dynarmic::GetExclusiveMonitorValuePointer(&monitor_, 0))));
        exclusive_monitor_values_link_cell_address_ =
            execution_context_->link_cell_address(exclusive_monitor_values_link_cell_);
    }

    ~JitExecutor() {
        // Deregister the executor from the shared slab before invalidating
        // link cells that retired native code could otherwise still read.
        callbacks_->flush_translation_profile_recorder();
        const bool had_jit = static_cast<bool>(jit_);
        jit_.reset();
        execution_context_->unlink(runtime_link_cell_);
        execution_context_->unlink(lookup_link_cell_);
        execution_context_->unlink(runtime_config_link_cell_);
        execution_context_->unlink(fast_dispatch_table_link_cell_);
        execution_context_->unlink(page_table_link_cell_);
        execution_context_->unlink(read_page_table_link_cell_);
        execution_context_->unlink(exclusive_monitor_lock_link_cell_);
        execution_context_->unlink(exclusive_monitor_addresses_link_cell_);
        execution_context_->unlink(exclusive_monitor_values_link_cell_);
        performance_counters().record_jit_executor_memory_usage(
            execution_context_->context_id(), process_id_,
            static_cast<std::uint32_t>(execution_slot_), 0);
        if (had_jit) {
            performance_counters().record_jit_destroyed();
        }
    }

    CpuRunResult run(
        Cpu& cpu, std::uint64_t ticks, bool single_step,
        bool cooperative_execution,
        std::chrono::nanoseconds host_slice_budget =
            default_host_cooperative_slice_budget) {
        CpuRunPhaseDiagnostics diagnostics{
            process_id_, static_cast<std::uint32_t>(cpu.processor_id()),
            static_cast<std::uint32_t>(execution_slot_),
            single_step ? 1U : ticks};
        const std::unique_lock lock{execution_mutex_};
        diagnostics.checkpoint(PerfLatencyKind::CpuRunLockWait);
        memory_.synchronize_shared_write_tracking();
        diagnostics.checkpoint(PerfLatencyKind::CpuRunSharedWriteSync);
        ensure_jit();
        diagnostics.checkpoint(PerfLatencyKind::CpuRunEnsureJit);
        service_pending_shared_invalidation();
        observe_shared_invalidation_epoch();
        diagnostics.checkpoint(PerfLatencyKind::CpuRunInvalidation);
        load_state(cpu);
        diagnostics.checkpoint(PerfLatencyKind::CpuRunLoadState);
        const auto effective_host_slice_budget = std::max(
            host_slice_budget, std::chrono::nanoseconds::zero());
        callbacks_->begin(
            single_step ? 1 : ticks,
            cooperative_execution && !single_step,
            effective_host_slice_budget);
        diagnostics.checkpoint(PerfLatencyKind::CpuRunCallbacksBegin);
        try {
            const auto entry_location = single_step
                                             ? 0U
                                             : current_location_descriptor();
            if (!single_step) {
                preload_current_artifact();
            }
            diagnostics.checkpoint(PerfLatencyKind::CpuRunArtifactPreload);
            const auto reason = single_step ? jit_->Step() : jit_->Run();
            diagnostics.checkpoint(PerfLatencyKind::CpuRunExecute);
            record_dispatch_counters();
            auto result = callbacks_->result(reason);
            if (entry_location != 0U && result.ticks_consumed != 0U) {
                mark_native_preimport_used(entry_location);
                callbacks_->finish_demand_artifact(entry_location);
            }
            if (guest_preemption_requested_) {
                performance_counters().record_scheduler_preemption_return();
                if (guest_preemption_requested_at_) {
                    const auto elapsed =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() -
                            *guest_preemption_requested_at_)
                            .count();
                    performance_counters().record_latency(
                        PerfLatencyKind::SchedulerPreemptionRequestToReturn,
                        static_cast<std::uint64_t>(std::max<std::int64_t>(
                            0, elapsed)));
                }
                guest_preemption_requested_ = false;
                guest_preemption_requested_at_.reset();
            }
            diagnostics.record_result(
                result.ticks_consumed, result.host_yield_checks,
                result.host_yielded, result.svc_calls, result.svc);
            performance_counters().record_jit_host_yield(
                result.host_yield_checks, result.host_yielded);
            if (cooperative_execution && !single_step) {
                performance_counters().record_jit_host_slice_budget(
                    static_cast<std::uint64_t>(
                        effective_host_slice_budget.count()));
            }
            diagnostics.checkpoint(PerfLatencyKind::CpuRunResult);
            save_state(cpu);
            diagnostics.checkpoint(PerfLatencyKind::CpuRunSaveState);
            callbacks_->flush_translation_profile_recorder();
            record_code_cache_usage();
            performance_counters().record_cpu_execution(result.ticks_consumed);
            diagnostics.checkpoint(PerfLatencyKind::CpuRunCacheAccounting);
            return result;
        } catch (...) {
            guest_preemption_requested_ = false;
            guest_preemption_requested_at_.reset();
            record_dispatch_counters();
            save_state(cpu);
            callbacks_->flush_translation_profile_recorder();
            record_code_cache_usage();
            throw;
        }
    }

    [[nodiscard]] std::uint64_t code_cache_used() {
        const std::lock_guard lock{execution_mutex_};
        return jit_ ? jit_code_cache_used(*jit_) : 0U;
    }

    void clear_halt() {
        // Clearing Dynarmic's AST must also retire the matching request
        // bookkeeping. Otherwise a wake that is consumed while servicing a
        // Mach event can leave the next run looking like a continuation of
        // the old preemption request.
        guest_preemption_requested_ = false;
        guest_preemption_requested_at_.reset();
        if (jit_) {
            jit_->ClearHalt(all_halt_reasons());
        }
    }

    void halt(Dynarmic::HaltReason reason) {
        if (jit_) {
            jit_->HaltExecution(reason);
        }
    }

    void request_guest_preemption() {
        if (!guest_preemption_requested_) {
            guest_preemption_requested_ = true;
            if (performance_counters().cpu_source_diagnostics_enabled()) {
                guest_preemption_requested_at_ =
                    std::chrono::steady_clock::now();
            }
        }
        halt(Dynarmic::HaltReason::UserDefined2);
    }

    [[nodiscard]] bool guest_preemption_requested() const noexcept {
        return guest_preemption_requested_;
    }

    void raise_memory_fault(std::uint32_t address, std::size_t size,
                            MemoryPermission access) {
        if (jit_ != nullptr) {
            callbacks_->raise_memory_fault(address, size, access);
        }
    }

    void clear_exclusive_state() {
        ensure_jit();
        jit_->ClearExclusiveState();
        monitor_.ClearProcessor(processor_id_);
    }

    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile) {
        const std::lock_guard execution_lock{execution_mutex_};
        callbacks_->set_translation_profile(std::move(profile));
    }

    void set_artifact_retention(JitArtifactRetention retention) {
        const std::lock_guard execution_lock{execution_mutex_};
        callbacks_->set_artifact_retention(retention);
    }

    PrecompileDisposition precompile_descriptor(
        std::uint64_t descriptor, JitPrecompileTarget target,
        bool profile_derived) {
        const std::lock_guard execution_lock{execution_mutex_};
        memory_.synchronize_shared_write_tracking();
        ensure_jit();
        service_pending_shared_invalidation();
        observe_shared_cache_state();
        constexpr std::size_t cache_reserve = 8U * 1024U * 1024U;
        if (target == JitPrecompileTarget::NativeCode &&
            jit_code_cache_used(*jit_) + cache_reserve >= code_cache_size_) {
            return PrecompileDisposition::CacheFull;
        }
        const auto pc = static_cast<std::uint32_t>(descriptor);
        const auto code_address = pc & ~std::uint32_t{3};
        if (!memory_.accessible(code_address, sizeof(std::uint32_t),
                                MemoryPermission::Execute)) {
            return PrecompileDisposition::Deferred;
        }
        if (!memory_.translation_profile_stable(
                code_address, sizeof(std::uint32_t))) {
            callbacks_->discard_translation_location(descriptor);
            return PrecompileDisposition::Unstable;
        }
        const auto key = callbacks_->artifact_key(descriptor);
        const auto probe = key ? artifact_probes_.find(descriptor)
                               : artifact_probes_.end();
        if (target == JitPrecompileTarget::NativeCode && key &&
            probe != artifact_probes_.end() && probe->second.matches(*key)) {
            return PrecompileDisposition::ArtifactProbeHit;
        }
        if (target == JitPrecompileTarget::PortableIr) {
            auto available = callbacks_->artifact_available(descriptor);
            if (available) {
                if (profile_derived) {
                    callbacks_->note_profile_portable_existence_hit();
                }
                return PrecompileDisposition::PortableArtifactHit;
            }
            if (!available) {
                callbacks_->begin(0);
                available = callbacks_->generate_portable_artifact(
                    *jit_, descriptor);
            }
            record_code_cache_usage();
            if (profile_derived && available) {
                callbacks_->note_profile_portable_generated();
            }
            return available ? PrecompileDisposition::PortableGenerated
                             : PrecompileDisposition::Failed;
        }
        const bool before_first_demand =
            !callbacks_->demand_location_seen(descriptor);
        if (profile_derived) {
            callbacks_->note_native_preimport_attempted();
        }
        const auto imported = callbacks_->import_artifact(*jit_, descriptor);
        if (imported == JitCallbacks::ArtifactImportOutcome::Imported) {
            if (profile_derived) {
                callbacks_->note_native_preimport_imported();
                if (before_first_demand) {
                    callbacks_->note_native_preimport_before_first_demand();
                    callbacks_->note_profile_imported_before_first_run();
                }
            }
            if (key) {
                artifact_probes_[descriptor] = ArtifactProbe{
                    key->content_identity, key->layout_identity, true,
                    callbacks_->artifact_publication_generation()};
            }
            if (profile_derived && before_first_demand) {
                mark_native_preimported(descriptor);
            }
            record_code_cache_usage();
            return PrecompileDisposition::ArtifactImported;
        }
        if (imported == JitCallbacks::ArtifactImportOutcome::AlreadyPresent) {
            if (profile_derived) {
                callbacks_->note_native_preimport_already_present();
                if (before_first_demand) {
                    callbacks_->note_native_preimport_before_first_demand();
                    callbacks_->note_profile_imported_before_first_run();
                }
            }
            if (key) {
                artifact_probes_[descriptor] = ArtifactProbe{
                    key->content_identity, key->layout_identity, true,
                    callbacks_->artifact_publication_generation()};
            }
            if (profile_derived && before_first_demand) {
                mark_native_preimported(descriptor);
            }
            record_code_cache_usage();
            return PrecompileDisposition::SharedSlabHit;
        }
        bool newly_emitted{};
        try {
            const auto block_started = std::chrono::steady_clock::now();
            callbacks_->set_explicit_artifact_publication(true);
            callbacks_->begin(0);
            newly_emitted = jit_->Precompile(descriptor);
            callbacks_->set_explicit_artifact_publication(false);
            performance_counters().record_jit_block_compile(
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - block_started)
                        .count()));
        } catch (...) {
            callbacks_->set_explicit_artifact_publication(false);
            return PrecompileDisposition::Failed;
        }
        if (key) {
            artifact_probes_[descriptor] = ArtifactProbe{
                key->content_identity, key->layout_identity,
                imported == JitCallbacks::ArtifactImportOutcome::Imported ||
                    imported ==
                        JitCallbacks::ArtifactImportOutcome::AlreadyPresent,
                callbacks_->artifact_publication_generation()};
        }
        record_code_cache_usage();
        return newly_emitted ? PrecompileDisposition::NativeCompiled
                             : PrecompileDisposition::SharedSlabHit;
    }

    void reset_live_state() {
        ensure_jit();
        guest_preemption_requested_ = false;
        guest_preemption_requested_at_.reset();
        callbacks_->discard_demand_artifact();
        demand_artifact_probes_.clear();
        native_preimport_tracker_->clear();
        callbacks_->clear_demand_locations();
        jit_->Reset();
    }

    [[nodiscard]] std::array<std::uint32_t, 16>& registers() {
        return jit_->Regs();
    }

    [[nodiscard]] const std::array<std::uint32_t, 16>& registers() const {
        return jit_->Regs();
    }

    [[nodiscard]] std::array<std::uint32_t, 64>& extension_registers() {
        return jit_->ExtRegs();
    }

    [[nodiscard]] const std::array<std::uint32_t, 64>&
    extension_registers() const {
        return jit_->ExtRegs();
    }

    [[nodiscard]] std::uint32_t cpsr() const {
        return jit_->Cpsr();
    }

    void set_cpsr(std::uint32_t value) {
        jit_->SetCpsr(value);
    }

    [[nodiscard]] std::uint32_t fpscr() const {
        return jit_->Fpscr();
    }

    void set_fpscr(std::uint32_t value) {
        jit_->SetFpscr(value);
    }

private:
    static Dynarmic::IR::Block* portable_ir_demand_provider(
        void* user_arg, std::uint64_t location_descriptor,
        std::uint64_t slab_generation) noexcept {
        auto& executor = *static_cast<JitExecutor*>(user_arg);
        return executor.callbacks_->take_demand_artifact(
            location_descriptor, slab_generation);
    }

    struct ArtifactProbe {
        ContentIdentity content_identity;
        ContentIdentity layout_identity;
        bool imported{};
        std::uint64_t publication_generation{};

        [[nodiscard]] bool matches(const JitArtifactKey& key) const noexcept {
            return imported && content_identity == key.content_identity &&
                   layout_identity == key.layout_identity;
        }
    };

    struct DemandArtifactProbe {
        JitArtifactKey key;
        std::uint64_t publication_generation{};
        std::uint64_t slab_generation{};

        [[nodiscard]] bool matches(
            const JitArtifactKey& candidate,
            std::uint64_t candidate_publication_generation,
            std::uint64_t candidate_slab_generation) const noexcept {
            return key == candidate &&
                   publication_generation == candidate_publication_generation &&
                   slab_generation == candidate_slab_generation;
        }
    };

    void mark_native_preimported(std::uint64_t location_descriptor) noexcept {
        native_preimport_tracker_->mark(location_descriptor);
    }

    void mark_native_preimport_used(
        std::uint64_t location_descriptor) noexcept {
        if (native_preimport_tracker_->has_ready() &&
            native_preimport_tracker_->consume(location_descriptor)) {
            callbacks_->note_native_preimport_used();
        }
    }

    [[nodiscard]] std::uint64_t current_location_descriptor() const {
        const Dynarmic::A32::LocationDescriptor descriptor{
            jit_->Regs()[15], Dynarmic::A32::PSR{jit_->Cpsr()},
            Dynarmic::A32::FPSCR{jit_->Fpscr()}};
        return static_cast<Dynarmic::IR::LocationDescriptor>(descriptor).Value();
    }

    void preload_current_artifact() {
        const auto location = current_location_descriptor();
        const auto key = callbacks_->artifact_key(location);
        if (!key) return;
        const auto slab_generation =
            execution_context_->native_code_slab()->generation();
        if (callbacks_->demand_artifact_staged(location, slab_generation) ||
            callbacks_->demand_artifact_native_ready(
                location, slab_generation)) {
            return;
        }
        const auto publication_generation =
            callbacks_->artifact_publication_generation();
        if (const auto probe = demand_artifact_probes_.find(location);
            probe != demand_artifact_probes_.end() &&
            probe->second.matches(
                *key, publication_generation, slab_generation)) {
            return;
        }
        callbacks_->stage_demand_artifact(location, slab_generation);
        demand_artifact_probes_.insert_or_assign(
            location, DemandArtifactProbe{
                          *key, publication_generation, slab_generation});
    }

    void observe_shared_invalidation_epoch() {
        const auto invalidation_epoch =
            execution_context_->cache_invalidation_epoch();
        if (invalidation_epoch == observed_invalidation_epoch_) return;
        artifact_probes_.clear();
        demand_artifact_probes_.clear();
        native_preimport_tracker_->clear();
        callbacks_->clear_demand_locations();
        callbacks_->discard_demand_artifact();
        observed_invalidation_epoch_ = invalidation_epoch;
    }

    void service_pending_shared_invalidation() {
        if (execution_context_->cache_invalidation_epoch() ==
            observed_invalidation_epoch_) {
            return;
        }
        execution_context_->native_code_slab()->service_pending_invalidation();
        const auto slab_generation =
            execution_context_->native_code_slab()->generation();
        if (execution_context_->observe_slab_generation(slab_generation)) {
            performance_counters().record_jit_slab_generation_transition();
        }
    }

    void observe_shared_cache_state() {
        observe_shared_invalidation_epoch();
        const auto slab_generation =
            execution_context_->native_code_slab()->generation();
        if (slab_generation == observed_slab_generation_) return;
        // Capacity transitions are internal to the shared slab and do not
        // publish a guest invalidation epoch. Retire probes when a
        // precompile operation reaches this slower, serialized boundary.
        artifact_probes_.clear();
        demand_artifact_probes_.clear();
        native_preimport_tracker_->clear();
        callbacks_->clear_demand_locations();
        callbacks_->discard_demand_artifact();
        observed_slab_generation_ = slab_generation;
    }

    [[nodiscard]] static constexpr Dynarmic::HaltReason all_halt_reasons() {
        return Dynarmic::HaltReason::CacheInvalidation |
               Dynarmic::HaltReason::MemoryAbort |
               Dynarmic::HaltReason::UserDefined1 |
               Dynarmic::HaltReason::UserDefined2 |
               Dynarmic::HaltReason::UserDefined3 |
               Dynarmic::HaltReason::UserDefined4 |
               Dynarmic::HaltReason::UserDefined5 |
               Dynarmic::HaltReason::UserDefined6 |
               Dynarmic::HaltReason::UserDefined7 |
               Dynarmic::HaltReason::UserDefined8;
    }

    void ensure_jit() {
        if (jit_) {
            return;
        }
        if (runtime_link_cell_address_->load(std::memory_order_acquire) !=
            static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(callbacks_.get()))) {
            throw std::logic_error{
                "JIT runtime callback link is not bound"};
        }
        Dynarmic::A32::UserConfig config{callbacks_.get()};
        config.native_code_slab = execution_context_->native_code_slab();
        config.callbacks_link = runtime_link_cell_address_;
        config.lookup_link = lookup_link_cell_address_;
        config.runtime_config_link = runtime_config_link_cell_address_;
        config.fast_dispatch_table_link = fast_dispatch_table_link_cell_address_;
        config.page_table_link = page_table_link_cell_address_;
        config.read_page_table_link = read_page_table_link_cell_address_;
        config.coprocessor_user_arg_link = runtime_link_cell_address_;
        config.exclusive_monitor_lock_link =
            exclusive_monitor_lock_link_cell_address_;
        config.exclusive_monitor_addresses_link =
            exclusive_monitor_addresses_link_cell_address_;
        config.exclusive_monitor_values_link =
            exclusive_monitor_values_link_cell_address_;
        config.processor_id = processor_id_;
        config.global_monitor = &monitor_;
        config.arch_version = dynarmic_architecture_version(
            callbacks_->cpu_model().architecture_version());
        config.always_little_endian = true;
        config.enable_cycle_counting = true;
        config.check_halt_on_memory_access = true;
        config.code_cache_size = code_cache_size_;
        config.coprocessors[15] = cp15_;
        using DynarmicPageTable = std::array<
            std::uint8_t*,
            Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
        static_assert(
            AddressSpace::page_count ==
            Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES);
        auto** read_table = callbacks_->jit_read_page_table();
        auto** write_table = callbacks_->jit_write_page_table();
        if (read_table || write_table) {
            execution_context_->link(
                read_page_table_link_cell_,
                static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(read_table)));
            execution_context_->link(
                page_table_link_cell_,
                static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(write_table)));
            config.read_page_table =
                reinterpret_cast<DynarmicPageTable*>(read_table);
            config.page_table =
                reinterpret_cast<DynarmicPageTable*>(write_table);
            config.absolute_offset_page_table =
                sizeof(std::uintptr_t) >= sizeof(std::uint64_t);
            config.detect_misaligned_access_via_page_table =
                static_cast<std::uint8_t>(8U | 16U | 32U | 64U);
            config.only_detect_misalignment_via_page_table_on_page_boundary =
                true;
        }
        const auto measure = performance_counters().enabled();
        const auto started = measure
                                 ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
        jit_ = std::make_unique<Dynarmic::A32::Jit>(config);
        jit_->SetPortableIRDemandProvider(
            &JitExecutor::portable_ir_demand_provider, this);
        recorded_dispatch_counters_ = {};
        recorded_shared_cache_state_ = false;
        recorded_shared_range_count_ = 0;
        recorded_shared_descriptor_count_ = 0;
        recorded_invalidated_descriptors_ = 0;
        recorded_retired_code_bytes_ = 0;
        const auto elapsed =
            measure
                ? static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count())
                : 0;
        performance_counters().record_jit(elapsed);
        performance_counters().record_latency(
            PerfLatencyKind::JitColdPath, elapsed);
        record_code_cache_usage();
    }

    void load_state(Cpu& cpu) {
        clear_halt();
        jit_->Regs() = cpu.state_.registers;
        jit_->ExtRegs() = cpu.state_.extension_registers;
        jit_->SetCpsr(cpu.state_.cpsr);
        jit_->SetFpscr(cpu.state_.fpscr);
        cpu.active_executor_ = this;
        callbacks_->attach(&cpu, jit_.get());
        if (static_cast<std::uint32_t>(cpu.requested_halt_reason_) != 0) {
            jit_->HaltExecution(cpu.requested_halt_reason_);
        }
    }

    void save_state(Cpu& cpu) {
        cpu.state_.registers = jit_->Regs();
        cpu.state_.extension_registers = jit_->ExtRegs();
        cpu.state_.cpsr = jit_->Cpsr();
        cpu.state_.fpscr = jit_->Fpscr();
        cpu.active_executor_ = nullptr;
    }

    void record_code_cache_usage() {
        if (!jit_ || !performance_counters().enabled()) {
            return;
        }
        const auto current = jit_code_cache_used(*jit_);
        const auto committed = logical_committed_code_bytes(current);
        if (!recorded_shared_memory_ ||
            recorded_shared_used_bytes_ != current ||
            recorded_shared_committed_bytes_ != committed) {
            performance_counters().record_jit_shared_slab_usage(
                execution_context_->context_id(), code_cache_size_, committed,
                current);
            recorded_shared_memory_ = true;
            recorded_shared_used_bytes_ = current;
            recorded_shared_committed_bytes_ = committed;
        }
        const auto cache_stats =
            execution_context_->native_code_slab()->GetCacheStats();
        if (!recorded_shared_cache_state_ ||
            recorded_shared_range_count_ != cache_stats.range_count ||
            recorded_shared_descriptor_count_ !=
                cache_stats.descriptor_count ||
            recorded_invalidated_descriptors_ !=
                cache_stats.invalidated_descriptors ||
            recorded_retired_code_bytes_ != cache_stats.retired_code_bytes) {
            performance_counters().record_jit_shared_cache_state(
                execution_context_->context_id(),
                static_cast<std::uint64_t>(cache_stats.range_count),
                static_cast<std::uint64_t>(cache_stats.descriptor_count),
                cache_stats.invalidated_descriptors,
                cache_stats.retired_code_bytes);
            recorded_shared_cache_state_ = true;
            recorded_shared_range_count_ = cache_stats.range_count;
            recorded_shared_descriptor_count_ = cache_stats.descriptor_count;
            recorded_invalidated_descriptors_ =
                cache_stats.invalidated_descriptors;
            recorded_retired_code_bytes_ = cache_stats.retired_code_bytes;
        }
        const auto executor_local = executor_local_memory_bytes();
        if (executor_local != recorded_executor_local_bytes_) {
            performance_counters().record_jit_executor_memory_usage(
                execution_context_->context_id(), process_id_,
                static_cast<std::uint32_t>(execution_slot_), executor_local);
            recorded_executor_local_bytes_ = executor_local;
        }
    }

    void record_dispatch_counters() {
        if (!jit_) return;
        const auto current = jit_->GetDispatchCounters();
        if (!performance_counters().enabled()) {
            recorded_dispatch_counters_ = current;
            return;
        }
        const auto delta = [](std::uint64_t current,
                              std::uint64_t& recorded) {
            const auto result = current >= recorded ? current - recorded
                                                      : current;
            recorded = current;
            return result;
        };
        performance_counters().record_jit_dispatch(
            delta(current.fast_link_hits,
                  recorded_dispatch_counters_.fast_link_hits),
            delta(current.fast_link_misses,
                  recorded_dispatch_counters_.fast_link_misses),
            delta(current.stable_table_probes,
                  recorded_dispatch_counters_.stable_table_probes),
            delta(current.stable_table_collisions,
                  recorded_dispatch_counters_.stable_table_collisions),
            delta(current.rsb_hits, recorded_dispatch_counters_.rsb_hits),
            delta(current.rsb_misses,
                  recorded_dispatch_counters_.rsb_misses));
    }

    [[nodiscard]] std::uint64_t executor_local_memory_bytes() const noexcept {
        constexpr auto link_cell_bytes =
            jit_link_cell_count * sizeof(std::atomic<std::uint64_t>);
        if (!jit_) return link_cell_bytes;
        constexpr auto fast_dispatch_table_bytes =
            fast_dispatch_table_size * fast_dispatch_entry_bytes;
        // A32JitState includes the executor's RSB arrays.  The link cells are
        // allocated by the shared ExecutionContext but their payload is still
        // executor-local mutable state and is counted here exactly once.
        return link_cell_bytes + sizeof(Dynarmic::Backend::X64::A32JitState) +
               fast_dispatch_table_bytes;
    }

  public:
    void set_process_id(std::uint32_t process_id) {
        process_id_ = process_id;
    }

    void set_code_cache_size(std::size_t bytes) {
        std::lock_guard lock{execution_mutex_};
        if (jit_) {
            throw std::logic_error{
                "cannot resize a live Dynarmic code cache"};
        }
        code_cache_size_ = bytes;
    }

  private:
    std::size_t processor_id_{};
    std::size_t execution_slot_{};
    std::uint32_t process_id_{};
    AddressSpace& memory_;
    Dynarmic::ExclusiveMonitor& monitor_;
    std::unique_ptr<JitCallbacks> callbacks_;
    std::shared_ptr<ArmSystemControlCoprocessor> cp15_;
    std::shared_ptr<ExecutionContext> execution_context_;
    std::size_t runtime_link_cell_{};
    const std::atomic<std::uint64_t> *runtime_link_cell_address_{};
    std::size_t lookup_link_cell_{};
    std::atomic<std::uint64_t> *lookup_link_cell_address_{};
    std::size_t runtime_config_link_cell_{};
    std::atomic<std::uint64_t> *runtime_config_link_cell_address_{};
    std::size_t fast_dispatch_table_link_cell_{};
    std::atomic<std::uint64_t> *fast_dispatch_table_link_cell_address_{};
    std::size_t page_table_link_cell_{};
    std::atomic<std::uint64_t> *page_table_link_cell_address_{};
    std::size_t read_page_table_link_cell_{};
    std::atomic<std::uint64_t> *read_page_table_link_cell_address_{};
    std::size_t exclusive_monitor_lock_link_cell_{};
    const std::atomic<std::uint64_t> *exclusive_monitor_lock_link_cell_address_{};
    std::size_t exclusive_monitor_addresses_link_cell_{};
    const std::atomic<std::uint64_t> *exclusive_monitor_addresses_link_cell_address_{};
    std::size_t exclusive_monitor_values_link_cell_{};
    const std::atomic<std::uint64_t> *exclusive_monitor_values_link_cell_address_{};
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    std::size_t code_cache_size_{64U * 1024U * 1024U};
    bool recorded_shared_memory_{};
    std::uint64_t recorded_shared_used_bytes_{};
    std::uint64_t recorded_shared_committed_bytes_{};
    bool recorded_shared_cache_state_{};
    std::uint64_t recorded_shared_range_count_{};
    std::uint64_t recorded_shared_descriptor_count_{};
    std::uint64_t recorded_invalidated_descriptors_{};
    std::uint64_t recorded_retired_code_bytes_{};
    std::uint64_t recorded_executor_local_bytes_{};
    Dynarmic::A32::DispatchCounters recorded_dispatch_counters_{};
    std::uint64_t observed_invalidation_epoch_{};
    std::uint64_t observed_slab_generation_{};
    std::unordered_map<std::uint64_t, ArtifactProbe> artifact_probes_;
    std::unordered_map<std::uint64_t, DemandArtifactProbe>
        demand_artifact_probes_;
    std::shared_ptr<JitNativePreimportTracker> native_preimport_tracker_;
    bool guest_preemption_requested_{};
    std::optional<std::chrono::steady_clock::time_point>
        guest_preemption_requested_at_;
    std::mutex execution_mutex_;
};

class CpuExecutionPool {
    struct PrecompileEntry {
        std::uint64_t descriptor{};
        JitPrecompileTarget target{JitPrecompileTarget::NativeCode};

        friend constexpr bool operator==(const PrecompileEntry &,
                                         const PrecompileEntry &) = default;
    };

    struct PrecompileEntryHash {
        [[nodiscard]] std::size_t operator()(
            const PrecompileEntry &entry) const noexcept {
            const auto descriptor_hash = std::hash<std::uint64_t>{}(
                entry.descriptor);
            const auto target_hash = std::hash<std::uint8_t>{}(
                static_cast<std::uint8_t>(entry.target));
            return descriptor_hash ^
                   (target_hash + static_cast<std::size_t>(0x9e3779b9U) +
                    (descriptor_hash << 6U) + (descriptor_hash >> 2U));
        }
    };

    struct DeferredPrecompileEntry {
        JitPrecompilePhase phase{JitPrecompilePhase::Remaining};
        // CacheFull is retryable only after the shared slab advances to a new
        // generation. Ordinary Deferred entries leave this empty and are
        // retried when the profile is refreshed or the mapping is re-added.
        std::optional<std::uint64_t> cache_full_generation;
    };

public:
    CpuExecutionPool(
        AddressSpace& memory,
        Dynarmic::ExclusiveMonitor& monitor,
        std::size_t execution_slot_count,
        std::size_t first_processor_id,
        const ArmCpuModel& cpu_model,
        std::shared_ptr<JitArtifactStore> artifact_store)
        : memory_{memory},
          execution_context_{std::make_shared<ExecutionContext>()},
          native_preimport_tracker_{
              std::make_shared<JitNativePreimportTracker>()} {
        if (execution_slot_count == 0) {
            throw std::invalid_argument{
                "execution_slot_count must be at least one"};
        }
        if (first_processor_id > monitor.GetProcessorCount() ||
            execution_slot_count >
                monitor.GetProcessorCount() - first_processor_id) {
            throw std::invalid_argument{
                "exclusive monitor processor range is out of bounds"};
        }
        profile_locations_.reserve(jit_translation_profile_maximum_locations);
        executors_.reserve(execution_slot_count);
        for (std::size_t slot = 0; slot < execution_slot_count; ++slot) {
            executors_.push_back(std::make_unique<JitExecutor>(
                first_processor_id + slot, slot, memory, monitor, cpu_model,
                artifact_store, execution_context_, native_preimport_tracker_));
        }
    }

    ~CpuExecutionPool() {
        quiesce_precompilation();
        // Clear the executors before dropping the shared accounting record.
        // Their destructors publish zero for their local slots; the final
        // release then removes the one shared slab entry without leaving a
        // stale reservation behind.
        executors_.clear();
        performance_counters().release_jit_memory_context(
            execution_context_->context_id());
    }

    [[nodiscard]] std::size_t size() const {
        return executors_.size();
    }

    [[nodiscard]] JitExecutor& executor(std::size_t slot) {
        return *executors_.at(slot);
    }

    void set_process_id(std::uint32_t process_id) {
        execution_context_->bind_process_id(process_id);
        for (auto& executor : executors_)
            executor->set_process_id(process_id);
    }

    void set_code_cache_size(std::size_t bytes) {
        for (auto& executor : executors_)
            executor->set_code_cache_size(bytes);
    }

    [[nodiscard]] std::uint64_t code_cache_used() {
        // All executors in this pool publish into one NativeCodeSlab.  Jit's
        // CodeCacheUsed therefore reports the same process-wide byte count
        // from every slot; summing it would multiply one allocation by the
        // number of guest CPUs.  Read each slot only until an initialized
        // shared slab reports a non-zero value, without creating a JIT merely
        // to obtain the accounting sample.
        for (auto& executor : executors_) {
            const auto used = executor->code_cache_used();
            if (used != 0U) return used;
        }
        return 0U;
    }

    void clear_cache() {
        static_cast<void>(execution_context_->request_cache_clear());
        performance_counters().record_jit_shared_invalidation(true);
    }

    void invalidate_cache_range(std::uint32_t address, std::size_t length) {
        if (length == 0U) return;
        static_cast<void>(
            execution_context_->request_cache_range(address, length));
        performance_counters().record_jit_shared_invalidation(false);
    }

    void disable_jit_page_table() {
        memory_.disable_jit_page_table();
    }

    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile,
        JitPrecompilePhase phase) {
        quiesce_precompilation();
        native_preimport_tracker_->clear();
        const auto locations =
            profile ? profile->snapshot() : std::vector<std::uint64_t>{};
        for (auto& executor : executors_) {
            executor->set_translation_profile(profile);
        }
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        for (auto &queue : pending_precompile_entries_) queue.clear();
        pending_precompile_phases_.clear();
        inflight_precompile_entries_.clear();
        completed_precompile_entries_.clear();
        deferred_precompile_entries_.clear();
        cache_full_generation_observed_.reset();
        next_precompile_executor_ = 0;
        translation_profile_ = profile;
        translation_profile_phase_ = phase;
        profile_locations_.clear();
        for (const auto location : locations) {
            if (location == 0U) continue;
            profile_locations_.insert(location);
            const auto native_queued = enqueue_precompile_entry_locked(
                PrecompileEntry{location, JitPrecompileTarget::NativeCode},
                phase);
            const auto portable_queued = enqueue_precompile_entry_locked(
                PrecompileEntry{location, JitPrecompileTarget::PortableIr},
                phase);
            if (portable_queued && profile) {
                profile->note_profile_enqueued_portable();
            }
            if (!native_queued || !portable_queued) {
                break;
            }
        }
    }

    void refresh_translation_profile() {
        const auto profile = translation_profile_;
        if (!profile) return;
        const auto locations = profile->snapshot();
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        for (const auto location : locations) {
            if (location == 0U || profile_locations_.contains(location)) {
                continue;
            }
            profile_locations_.insert(location);
            const auto native_queued = enqueue_precompile_entry_locked(
                PrecompileEntry{location, JitPrecompileTarget::NativeCode},
                translation_profile_phase_);
            const auto portable_queued = enqueue_precompile_entry_locked(
                PrecompileEntry{location, JitPrecompileTarget::PortableIr},
                translation_profile_phase_);
            if (portable_queued) {
                profile->note_profile_enqueued_portable();
            }
            if (!native_queued || !portable_queued) break;
        }
    }

    void set_artifact_retention(JitArtifactRetention retention) {
        for (auto& executor : executors_) {
            executor->set_artifact_retention(retention);
        }
    }

    void add_precompile_entries(
        const std::vector<std::uint64_t> &location_descriptors,
        JitPrecompilePhase phase) {
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        for (const auto entry : location_descriptors) {
            if (!enqueue_precompile_entry_locked(
                    PrecompileEntry{entry, JitPrecompileTarget::NativeCode},
                    phase) ||
                !enqueue_precompile_entry_locked(
                    PrecompileEntry{entry, JitPrecompileTarget::PortableIr},
                    phase)) {
                break;
            }
        }
    }

    [[nodiscard]] std::optional<JitPrecompilePhase>
    next_precompile_phase(JitPrecompileTarget target) {
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        return next_precompile_phase_locked(target);
    }

    JitPrecompileBatchResult precompile_pending(
        std::size_t maximum_blocks, std::uint64_t budget_nanoseconds,
        JitPrecompileTarget target,
        const CpuCluster::PrecompileStopCondition &stop_condition) {
        JitPrecompileBatchResult result;
        if (executors_.empty() || maximum_blocks == 0 ||
            budget_nanoseconds == 0) {
            return result;
        }
        std::uint64_t cancellation_generation{};
        {
            const std::lock_guard queue_lock{precompile_queue_mutex_};
            cancellation_generation = precompile_cancellation_generation_;
            ++active_precompile_tasks_;
        }
        std::unordered_set<PrecompileEntry, PrecompileEntryHash>
            owned_inflight_entries;
        const auto stop_requested = [&]() {
            if (stop_condition && stop_condition()) return true;
            const std::lock_guard queue_lock{precompile_queue_mutex_};
            return precompile_cancellation_generation_ !=
                   cancellation_generation;
        };
        const auto cleanup = [&]() {
            const std::lock_guard queue_lock{precompile_queue_mutex_};
            for (const auto &entry : owned_inflight_entries)
                inflight_precompile_entries_.erase(entry);
            owned_inflight_entries.clear();
        };
        const auto finish = [&]() {
            const std::lock_guard queue_lock{precompile_queue_mutex_};
            if (active_precompile_tasks_ == 0U)
                throw std::logic_error{"precompile task accounting underflow"};
            --active_precompile_tasks_;
            if (active_precompile_tasks_ == 0U)
                precompile_idle_.notify_all();
        };
        try {
        const auto started = std::chrono::steady_clock::now();
        const auto bounded_budget = std::min<std::uint64_t>(
            budget_nanoseconds,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::chrono::nanoseconds::rep>::max()));
        const auto deadline =
            started + std::chrono::nanoseconds{
                          static_cast<std::chrono::nanoseconds::rep>(
                              bounded_budget)};
        std::size_t processed = 0;
        while (processed < maximum_blocks) {
            if (stop_requested()) break;
            if (std::chrono::steady_clock::now() >= deadline) {
                ++result.deadline_stops;
                break;
            }
            std::optional<std::pair<PrecompileEntry, JitPrecompilePhase>> entry;
            std::size_t executor_index{};
            bool profile_derived{};
            {
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                entry = take_precompile_entry_locked(target);
                if (!entry) break;
                executor_index = next_precompile_executor_++ % executors_.size();
                profile_derived = profile_locations_.contains(
                    entry->first.descriptor);
            }
            owned_inflight_entries.insert(entry->first);
            ++processed;
            ++result.attempted;
            const auto precompile_entry = entry->first;
            const auto descriptor = precompile_entry.descriptor;
            JitExecutor::PrecompileDisposition disposition;
            try {
                disposition = executors_[executor_index]->precompile_descriptor(
                    descriptor, target, profile_derived);
            } catch (...) {
                const auto cancelled = stop_requested();
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                inflight_precompile_entries_.erase(precompile_entry);
                owned_inflight_entries.erase(precompile_entry);
                if (!cancelled) {
                    deferred_precompile_entries_[precompile_entry] =
                        DeferredPrecompileEntry{entry->second, std::nullopt};
                }
                ++result.failed;
                break;
            }
            const auto cancelled = stop_requested();
            {
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                inflight_precompile_entries_.erase(precompile_entry);
                owned_inflight_entries.erase(precompile_entry);
                if (cancelled) {
                    // The block may have finished after the epoch changed.
                    // It belongs to the old work generation and must not be
                    // published into the replacement profile's sets.
                } else if (disposition ==
                           JitExecutor::PrecompileDisposition::Deferred) {
                    deferred_precompile_entries_[precompile_entry] =
                        DeferredPrecompileEntry{entry->second, std::nullopt};
                } else if (disposition ==
                           JitExecutor::PrecompileDisposition::CacheFull) {
                    deferred_precompile_entries_[precompile_entry] =
                        DeferredPrecompileEntry{
                            entry->second,
                            execution_context_->native_code_slab()->generation()};
                } else {
                    completed_precompile_entries_.insert(precompile_entry);
                }
            }
            if (cancelled) break;
            switch (disposition) {
            case JitExecutor::PrecompileDisposition::NativeCompiled:
                ++result.native_compiled;
                break;
            case JitExecutor::PrecompileDisposition::PortableGenerated:
                ++result.portable_generated;
                break;
            case JitExecutor::PrecompileDisposition::PortableArtifactHit:
                ++result.portable_artifact_hits;
                break;
            case JitExecutor::PrecompileDisposition::ArtifactImported:
                ++result.artifact_imported;
                break;
            case JitExecutor::PrecompileDisposition::ArtifactProbeHit:
                ++result.artifact_probe_hits;
                break;
            case JitExecutor::PrecompileDisposition::SharedSlabHit:
                ++result.shared_slab_hits;
                break;
            case JitExecutor::PrecompileDisposition::Deferred:
                ++result.deferred;
                break;
            case JitExecutor::PrecompileDisposition::Unstable:
                ++result.unstable;
                break;
            case JitExecutor::PrecompileDisposition::CacheFull:
                ++result.cache_full;
                break;
            case JitExecutor::PrecompileDisposition::Failed:
                ++result.failed;
                break;
            }
            if (disposition == JitExecutor::PrecompileDisposition::CacheFull) {
                break;
            }
        }
        cleanup();
        finish();
        return result;
        } catch (...) {
            cleanup();
            finish();
            throw;
        }
    }

    void quiesce_precompilation() {
        {
            const std::lock_guard queue_lock{precompile_queue_mutex_};
            ++precompile_cancellation_generation_;
            for (auto &queue : pending_precompile_entries_) queue.clear();
            pending_precompile_phases_.clear();
            deferred_precompile_entries_.clear();
            completed_precompile_entries_.clear();
            cache_full_generation_observed_.reset();
            next_precompile_executor_ = 0;
        }
        std::unique_lock queue_lock{precompile_queue_mutex_};
        precompile_idle_.wait(queue_lock, [this] {
            return active_precompile_tasks_ == 0U;
        });
        inflight_precompile_entries_.clear();
    }

private:
    static constexpr std::size_t phase_index(JitPrecompilePhase phase) {
        return static_cast<std::size_t>(phase);
    }

    [[nodiscard]] bool enqueue_precompile_entry_locked(
        PrecompileEntry entry, JitPrecompilePhase phase) {
        if (const auto deferred = deferred_precompile_entries_.find(entry);
            deferred != deferred_precompile_entries_.end()) {
            if (phase_index(deferred->second.phase) < phase_index(phase)) {
                phase = deferred->second.phase;
            }
            deferred_precompile_entries_.erase(deferred);
        }
        if (entry.descriptor == 0 ||
            completed_precompile_entries_.contains(entry) ||
            inflight_precompile_entries_.contains(entry)) {
            return true;
        }
        if (const auto pending = pending_precompile_phases_.find(entry);
            pending != pending_precompile_phases_.end()) {
            if (phase_index(phase) < phase_index(pending->second)) {
                pending->second = phase;
                pending_precompile_entries_[phase_index(phase)].push_back(
                    entry);
            }
            return true;
        }
        if (pending_precompile_phases_.size() +
                inflight_precompile_entries_.size() +
                deferred_precompile_entries_.size() +
                completed_precompile_entries_.size() >=
            jit_translation_profile_maximum_locations *
                jit_precompile_target_count) {
            return false;
        }
        pending_precompile_phases_.emplace(entry, phase);
        pending_precompile_entries_[phase_index(phase)].push_back(entry);
        return true;
    }

    void promote_cache_full_entries_locked() {
        if (deferred_precompile_entries_.empty()) return;
        const auto current_generation =
            execution_context_->native_code_slab()->generation();
        if (cache_full_generation_observed_ &&
            *cache_full_generation_observed_ == current_generation) {
            return;
        }
        cache_full_generation_observed_ = current_generation;
        for (auto iterator = deferred_precompile_entries_.begin();
             iterator != deferred_precompile_entries_.end();) {
            if (!iterator->second.cache_full_generation ||
                *iterator->second.cache_full_generation == current_generation) {
                ++iterator;
                continue;
            }
            const auto entry = iterator->first;
            const auto phase = iterator->second.phase;
            pending_precompile_phases_.emplace(entry, phase);
            pending_precompile_entries_[phase_index(phase)].push_back(entry);
            iterator = deferred_precompile_entries_.erase(iterator);
        }
    }

    [[nodiscard]] std::optional<JitPrecompilePhase>
    next_precompile_phase_locked(JitPrecompileTarget target) {
        promote_cache_full_entries_locked();
        for (std::size_t index = 0; index < pending_precompile_entries_.size();
             ++index) {
            auto &queue = pending_precompile_entries_[index];
            for (auto iterator = queue.begin(); iterator != queue.end();) {
                const auto entry = *iterator;
                const auto pending = pending_precompile_phases_.find(entry);
                if (pending == pending_precompile_phases_.end() ||
                    phase_index(pending->second) != index) {
                    iterator = queue.erase(iterator);
                    continue;
                }
                if (entry.target == target) {
                    return pending->second;
                }
                ++iterator;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::pair<PrecompileEntry,
                                          JitPrecompilePhase>>
    take_precompile_entry_locked(JitPrecompileTarget target) {
        const auto phase = next_precompile_phase_locked(target);
        if (!phase) return std::nullopt;
        auto &queue = pending_precompile_entries_[phase_index(*phase)];
        for (auto iterator = queue.begin(); iterator != queue.end();) {
            const auto entry = *iterator;
            const auto pending = pending_precompile_phases_.find(entry);
            if (pending == pending_precompile_phases_.end() ||
                phase_index(pending->second) != phase_index(*phase)) {
                iterator = queue.erase(iterator);
                continue;
            }
            if (entry.target != target) {
                ++iterator;
                continue;
            }
            queue.erase(iterator);
            pending_precompile_phases_.erase(entry);
            inflight_precompile_entries_.insert(entry);
            return std::pair{entry, *phase};
        }
        return std::nullopt;
    }

    AddressSpace& memory_;
    std::shared_ptr<ExecutionContext> execution_context_;
    std::shared_ptr<JitNativePreimportTracker> native_preimport_tracker_;
    std::vector<std::unique_ptr<JitExecutor>> executors_;
    std::array<std::deque<PrecompileEntry>, jit_precompile_phase_count>
        pending_precompile_entries_;
    std::unordered_map<PrecompileEntry, JitPrecompilePhase,
                       PrecompileEntryHash>
        pending_precompile_phases_;
    std::unordered_set<PrecompileEntry, PrecompileEntryHash>
        inflight_precompile_entries_;
    std::unordered_map<PrecompileEntry, DeferredPrecompileEntry,
                       PrecompileEntryHash>
        deferred_precompile_entries_;
    std::unordered_set<PrecompileEntry, PrecompileEntryHash>
        completed_precompile_entries_;
    std::unordered_set<std::uint64_t> profile_locations_;
    std::shared_ptr<JitTranslationProfile> translation_profile_;
    JitPrecompilePhase translation_profile_phase_{
        JitPrecompilePhase::Remaining};
    std::optional<std::uint64_t> cache_full_generation_observed_;
    std::size_t next_precompile_executor_{};
    std::uint64_t precompile_cancellation_generation_{1};
    std::uint64_t active_precompile_tasks_{};
    std::condition_variable precompile_idle_;
    std::mutex precompile_queue_mutex_;
};

Cpu::Cpu(
    std::size_t processor_id, AddressSpace& memory, Dynarmic::ExclusiveMonitor& monitor)
    : Cpu{processor_id,
          std::make_shared<CpuExecutionPool>(
              memory, monitor, 1, processor_id, default_arm_cpu_model(),
              nullptr)} {}

Cpu::Cpu(
    std::size_t processor_id,
    std::shared_ptr<CpuExecutionPool> execution_pool)
    : processor_id_{processor_id},
      execution_pool_{std::move(execution_pool)} {}

Cpu::~Cpu() = default;

CpuRunResult Cpu::run(std::uint64_t ticks, std::size_t execution_slot) {
    if (!execution_pool_) {
        throw std::logic_error{"CPU execution resources have been released"};
    }
    return execution_pool_->executor(execution_slot).run(
        *this, ticks, false, false);
}

CpuRunResult Cpu::run_cooperatively(
    std::uint64_t ticks, std::size_t execution_slot) {
    if (!execution_pool_) {
        throw std::logic_error{"CPU execution resources have been released"};
    }
    return execution_pool_->executor(execution_slot).run(
        *this, ticks, false, true, default_host_cooperative_slice_budget);
}

CpuRunResult Cpu::run_cooperatively(
    std::uint64_t ticks, std::chrono::nanoseconds host_slice_budget,
    std::size_t execution_slot) {
    if (!execution_pool_) {
        throw std::logic_error{"CPU execution resources have been released"};
    }
    return execution_pool_->executor(execution_slot).run(
        *this, ticks, false, true, host_slice_budget);
}

CpuRunResult Cpu::step(std::size_t execution_slot) {
    if (!execution_pool_) {
        throw std::logic_error{"CPU execution resources have been released"};
    }
    return execution_pool_->executor(execution_slot).run(
        *this, 1, true, false);
}

void Cpu::reset() {
    state_ = {};
    if (active_executor_) {
        active_executor_->reset_live_state();
    }
}
void Cpu::clear_cache() {
    if (execution_pool_) {
        execution_pool_->clear_cache();
    }
}
void Cpu::invalidate_cache_range(std::uint32_t address, std::size_t length) {
    if (execution_pool_) {
        execution_pool_->invalidate_cache_range(address, length);
    }
}
void Cpu::raise_memory_fault(std::uint32_t address, std::size_t size,
                             MemoryPermission access) {
    if (active_executor_) {
        active_executor_->raise_memory_fault(address, size, access);
        return;
    }
    // A deferred SVC is dispatched after the executor has returned, so there
    // is no Dynarmic callback object available to carry MemoryFault. Preserve
    // the scheduler-visible fatal boundary in that case.
    halt(Dynarmic::HaltReason::UserDefined4);
}
void Cpu::clear_halt() {
    requested_halt_reason_ = {};
    if (active_executor_) {
        active_executor_->clear_halt();
    }
}
void Cpu::halt(Dynarmic::HaltReason reason) {
    requested_halt_reason_ = requested_halt_reason_ | reason;
    if (active_executor_) {
        active_executor_->halt(reason);
    }
}

void Cpu::request_guest_preemption() {
    performance_counters().record_scheduler_preemption_request();
    const bool coalesced =
        Dynarmic::Has(requested_halt_reason_, Dynarmic::HaltReason::UserDefined2) ||
        (active_executor_ != nullptr &&
         active_executor_->guest_preemption_requested());
    if (coalesced) {
        performance_counters().record_scheduler_preemption_coalesced();
    }
    requested_halt_reason_ =
        requested_halt_reason_ | Dynarmic::HaltReason::UserDefined2;
    if (active_executor_) {
        active_executor_->request_guest_preemption();
    }
}

Dynarmic::HaltReason Cpu::consume_requested_halt_reason() {
    const auto reason = requested_halt_reason_;
    requested_halt_reason_ = {};
    if (Dynarmic::Has(reason, Dynarmic::HaltReason::UserDefined2)) {
        performance_counters().record_scheduler_preemption_deferred_consume();
    }
    return reason;
}

std::array<std::uint32_t, 16>& Cpu::registers() {
    return active_executor_ ? active_executor_->registers()
                            : state_.registers;
}
const std::array<std::uint32_t, 16>& Cpu::registers() const {
    return active_executor_ ? active_executor_->registers()
                            : state_.registers;
}
std::uint32_t Cpu::cpsr() const {
    return active_executor_ ? active_executor_->cpsr() : state_.cpsr;
}
void Cpu::set_cpsr(std::uint32_t value) {
    if (active_executor_) {
        active_executor_->set_cpsr(value);
    } else {
        state_.cpsr = value;
    }
}
std::array<std::uint32_t, 64>& Cpu::extension_registers() {
    return active_executor_ ? active_executor_->extension_registers()
                            : state_.extension_registers;
}
const std::array<std::uint32_t, 64>& Cpu::extension_registers() const {
    return active_executor_ ? active_executor_->extension_registers()
                            : state_.extension_registers;
}
std::uint32_t Cpu::fpscr() const {
    return active_executor_ ? active_executor_->fpscr() : state_.fpscr;
}
void Cpu::set_fpscr(std::uint32_t value) {
    if (active_executor_) {
        active_executor_->set_fpscr(value);
    } else {
        state_.fpscr = value;
    }
}
std::optional<std::uint32_t> Cpu::cthread_self() const {
    return state_.cthread_self;
}
void Cpu::set_cthread_self(std::optional<std::uint32_t> value) {
    state_.cthread_self = value;
}
void Cpu::set_svc_handler(SvcHandler handler) {
    svc_handler_ = std::move(handler);
}
void Cpu::set_svc_dispatch_mode(SvcDispatchMode mode) {
    svc_dispatch_mode_ = mode;
}
void Cpu::set_memory_write_watchpoint(
    std::uint32_t address, MemoryWriteHandler handler) {
    if (handler && execution_pool_) {
        execution_pool_->disable_jit_page_table();
    }
    memory_write_watch_address_ = address;
    memory_write_handler_ = std::move(handler);
}
void Cpu::set_debug_breakpoints_enabled(bool enabled) {
    debug_breakpoints_enabled_ = enabled;
}
void Cpu::set_translation_profile(
    std::shared_ptr<JitTranslationProfile> profile) {
    if (execution_pool_) {
        execution_pool_->set_translation_profile(
            std::move(profile), JitPrecompilePhase::Remaining);
    }
}
void Cpu::clear_exclusive_state(std::size_t execution_slot) {
    if (!execution_pool_) {
        return;
    }
    // The local state gates STREX. The next LDREX overwrites this serialized
    // processor's single global slot, so clearing the local state is enough
    // and avoids taking the global monitor lock on every context switch.
    execution_pool_->executor(execution_slot).clear_exclusive_state();
}

CpuCluster::CpuCluster(std::size_t processor_count, AddressSpace& memory)
    : CpuCluster{processor_count, processor_count, memory} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory)
    : CpuCluster{
          initial_processor_count, maximum_processor_count, memory, false} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory,
    bool serialized_execution)
    : CpuCluster{
          initial_processor_count, maximum_processor_count, memory,
          serialized_execution, default_arm_cpu_model()} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory,
    bool serialized_execution,
    const ArmCpuModel& cpu_model)
    : CpuCluster{
          initial_processor_count, maximum_processor_count, memory,
          serialized_execution ? 1U : maximum_processor_count, cpu_model} {}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory,
    std::size_t execution_slot_count,
    const ArmCpuModel& cpu_model)
    : memory_{&memory},
      maximum_processor_count_{maximum_processor_count},
      serialized_execution_{execution_slot_count == 1},
      cpu_model_{&cpu_model},
      monitor_{execution_slot_count == 0 ? 1U : execution_slot_count},
      execution_monitor_{&monitor_},
      monitor_processor_base_{},
      execution_pool_{std::make_shared<CpuExecutionPool>(
          memory, *execution_monitor_, execution_slot_count,
          monitor_processor_base_, cpu_model, nullptr)} {
    if (initial_processor_count == 0) {
        throw std::invalid_argument{
            "initial_processor_count must be at least one"};
    }
    if (maximum_processor_count < initial_processor_count) {
        throw std::invalid_argument{
            "maximum_processor_count must cover the initial processors"};
    }
    cpus_.reserve(maximum_processor_count);
    while (cpus_.size() < initial_processor_count) {
        static_cast<void>(add_cpu());
    }
}

CpuCluster::CpuCluster(
    std::size_t initial_processor_count,
    std::size_t maximum_processor_count,
    AddressSpace& memory,
    std::size_t execution_slot_count,
    const ArmCpuModel& cpu_model,
    Dynarmic::ExclusiveMonitor& monitor,
    std::size_t monitor_processor_base,
    std::shared_ptr<JitArtifactStore> artifact_store,
    std::shared_ptr<GuestExclusiveAddressResolver> address_resolver)
    : memory_{&memory},
      maximum_processor_count_{maximum_processor_count},
      serialized_execution_{execution_slot_count == 1},
      cpu_model_{&cpu_model},
      monitor_{1U},
      execution_monitor_{&monitor},
      monitor_processor_base_{monitor_processor_base},
      monitor_processor_count_{execution_slot_count},
      address_resolver_{std::move(address_resolver)},
      execution_pool_{std::make_shared<CpuExecutionPool>(
          memory, *execution_monitor_, execution_slot_count,
          monitor_processor_base_, cpu_model, std::move(artifact_store))} {
    if (initial_processor_count == 0) {
        throw std::invalid_argument{
            "initial_processor_count must be at least one"};
    }
    if (maximum_processor_count < initial_processor_count) {
        throw std::invalid_argument{
            "maximum_processor_count must cover the initial processors"};
    }
    if (address_resolver_) {
        address_resolver_->bind(
            monitor_processor_base_, monitor_processor_count_, memory);
        monitor.SetAddressResolver(
            &GuestExclusiveAddressResolver::resolve_callback,
            address_resolver_.get());
        // A serialized physical CPU can revoke a page's direct-write entry
        // immediately before LDREX through the MemoryReadExclusive hook.
        // Multi-slot clusters keep all writes checked because another slot
        // may already be executing a direct store while that hook runs.
        if (monitor_processor_count_ > 1) {
            memory.disable_jit_write_page_table();
        }
    }
    if (!address_resolver_) {
        memory.set_exclusive_write_observer(
            [&monitor] { monitor.Clear(); });
    }
    cpus_.reserve(maximum_processor_count);
    while (cpus_.size() < initial_processor_count) {
        static_cast<void>(add_cpu());
    }
}

CpuCluster::~CpuCluster() {
    quiesce_precompilation();
    if (address_resolver_ != nullptr) {
        address_resolver_->unbind(
            monitor_processor_base_, monitor_processor_count_, *memory_);
    }
}

std::optional<std::size_t> CpuCluster::add_cpu() {
    if (cpus_.size() >= capacity()) {
        return std::nullopt;
    }
    const auto id = cpus_.size();
    cpus_.push_back(
        std::unique_ptr<Cpu>{new Cpu{id, execution_pool_}});
    return id;
}

void CpuCluster::set_process_id(std::uint32_t process_id) {
    execution_pool_->set_process_id(process_id);
}

void CpuCluster::set_jit_code_cache_size(std::size_t bytes) {
    execution_pool_->set_code_cache_size(bytes);
}

std::uint64_t CpuCluster::jit_code_cache_bytes() {
    return execution_pool_ ? execution_pool_->code_cache_used() : 0U;
}

void CpuCluster::clear_cache() {
    execution_pool_->clear_cache();
}

void CpuCluster::invalidate_cache_range(
    std::uint32_t address, std::size_t length) {
    execution_pool_->invalidate_cache_range(address, length);
}

void CpuCluster::set_translation_profile(
    std::shared_ptr<JitTranslationProfile> profile,
    JitPrecompilePhase phase) {
    execution_pool_->set_translation_profile(std::move(profile), phase);
}

void CpuCluster::refresh_translation_profile() {
    if (execution_pool_) execution_pool_->refresh_translation_profile();
}

void CpuCluster::set_jit_artifact_retention(
    JitArtifactRetention retention) {
    execution_pool_->set_artifact_retention(retention);
}

void CpuCluster::add_precompile_entries(
    const std::vector<std::uint64_t> &location_descriptors,
    JitPrecompilePhase phase) {
    if (execution_pool_) {
        execution_pool_->add_precompile_entries(location_descriptors, phase);
    }
}

std::optional<JitPrecompilePhase> CpuCluster::next_precompile_phase(
    JitPrecompileTarget target) {
    if (!execution_pool_) return std::nullopt;
    return execution_pool_->next_precompile_phase(target);
}

JitPrecompileBatchResult CpuCluster::precompile_pending(
    std::size_t maximum_blocks, std::uint64_t budget_nanoseconds,
    JitPrecompileTarget target,
    PrecompileStopCondition stop_condition) {
    if (!execution_pool_) {
        return {};
    }
    return execution_pool_->precompile_pending(
        maximum_blocks, budget_nanoseconds, target, stop_condition);
}

void CpuCluster::quiesce_precompilation() {
    if (execution_pool_)
        execution_pool_->quiesce_precompilation();
}

std::shared_ptr<CpuExecutionPool>
CpuCluster::release_execution_resources() {
    if (!execution_pool_) {
        return {};
    }
    quiesce_precompilation();
    for (const auto& cpu : cpus_) {
        if (cpu->active_executor_ != nullptr) {
            throw std::logic_error{
                "cannot release CPU execution resources while executing"};
        }
    }
    auto retired = std::move(execution_pool_);
    for (auto& cpu : cpus_) {
        cpu->execution_pool_.reset();
    }
    return retired;
}

std::vector<CpuRunResult> CpuCluster::run_parallel(std::uint64_t ticks_per_cpu) {
    if (!execution_pool_) {
        throw std::logic_error{
            "CPU execution resources have been released"};
    }
    if (serialized_execution_ && cpus_.size() > 1) {
        throw std::logic_error{
            "serialized CPU contexts cannot execute in parallel"};
    }
    std::vector<CpuRunResult> results(cpus_.size());
    std::vector<std::thread> workers;
    workers.reserve(cpus_.size());
    for (std::size_t index = 0; index < cpus_.size(); ++index) {
        workers.emplace_back([&, index] {
            results[index] = cpus_[index]->run(ticks_per_cpu, index);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return results;
}

}  // namespace ilemu
