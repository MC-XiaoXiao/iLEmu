#include "ilemu/cpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <deque>
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

constexpr std::uint32_t jit_artifact_hle_abi_version = 1U;
constexpr std::uint32_t jit_artifact_backend_abi_version = 2U;
constexpr std::uint64_t jit_artifact_codegen_options = 1U;
constexpr std::uint32_t jit_artifact_format_version = 7U;

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
    JitCallbacks(
        AddressSpace& memory,
        const ArmCpuModel& cpu_model,
        std::shared_ptr<JitArtifactStore> artifact_store)
        : memory_{memory},
          cpu_model_{cpu_model},
          artifact_store_{std::move(artifact_store)} {}

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
        if (translation_block_ == &ir.block) {
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

    [[nodiscard]] bool import_artifact(
        Dynarmic::A32::Jit& jit,
        std::uint64_t location_descriptor) const noexcept {
        auto block = validated_artifact_block(location_descriptor);
        if (!block) return false;
        try {
            static_cast<void>(jit.Precompile(std::move(*block)));
            return true;
        } catch (...) {
            return false;
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
            return std::nullopt;
        }
        try {
            const auto artifact = find_artifact(location_descriptor);
            if (!artifact || artifact->data.normalized_ir.empty()) {
                return std::nullopt;
            }
            if (!dependencies_match(*artifact)) return std::nullopt;
            auto block = deserialize_dynarmic_ir(artifact->data.normalized_ir);
            if (!block || block->Location().Value() != location_descriptor) {
                return std::nullopt;
            }
            return block;
        } catch (...) {
            return std::nullopt;
        }
    }

    void record_constant_dependency(
        std::uint32_t address, std::uint32_t size,
        std::uint64_t value) {
        if (translation_block_ == nullptr ||
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
        const auto pc = static_cast<std::uint32_t>(location_descriptor);
        if (translation_profile_ &&
            memory_.translation_profile_stable(
                pc, sizeof(std::uint32_t))) {
            translation_profile_->record(location_descriptor);
        }
        const auto published = publish_artifact(
            location_descriptor, translation_nanoseconds, optimized_block);
        if (portable_generation_location_ == location_descriptor) {
            portable_generation_published_ = published;
        }
    }

public:

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
    }
    std::uint64_t GetTicksRemaining() override { return ticks_remaining_; }
    std::uint64_t GetTicksForCode(
        bool is_thumb, Dynarmic::A32::VAddr address,
        std::uint32_t instruction) override {
        return cpu_model_.ticks_for_instruction(
            is_thumb, address, instruction);
    }

    void begin(std::uint64_t ticks) {
        ticks_remaining_ = ticks;
        consumed_ = 0;
        svc_.reset();
        svc_calls_ = 0;
        fault_.reset();
        breakpoint_.reset();
        exception_.clear();
    }

    CpuRunResult result(Dynarmic::HaltReason reason) const {
        return CpuRunResult{
            reason, consumed_, svc_, svc_calls_, fault_, breakpoint_,
            exception_};
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
        translation_profile_ = std::move(profile);
    }

    void set_artifact_retention(JitArtifactRetention retention) noexcept {
        artifact_retention_ = retention;
    }

private:
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
    std::optional<std::uint64_t> portable_generation_location_;
    bool portable_generation_published_{};
    Dynarmic::IR::Block *translation_block_{};
    std::vector<std::uint32_t> translation_code_pages_;
    std::vector<JitConstantDependency> translation_constant_dependencies_;
    bool constant_dependency_failed_{};
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
    JitExecutor(
        std::size_t processor_id,
        std::size_t execution_slot,
        AddressSpace& memory,
        Dynarmic::ExclusiveMonitor& monitor,
        const ArmCpuModel& cpu_model,
        std::shared_ptr<JitArtifactStore> artifact_store,
        std::shared_ptr<ExecutionContext> execution_context)
        : processor_id_{processor_id},
          execution_slot_{execution_slot},
          memory_{memory},
          monitor_{monitor},
          callbacks_{std::make_unique<JitCallbacks>(
              memory, cpu_model, std::move(artifact_store))},
          cp15_{std::make_unique<ArmSystemControlCoprocessor>(
              *callbacks_)},
          execution_context_{std::move(execution_context)} {
        if (!execution_context_) {
            throw std::invalid_argument{
                "JIT executor requires an execution context"};
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
        page_table_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(page_table_link_cell_, 0);
        page_table_link_cell_address_ =
            execution_context_->link_cell_address(page_table_link_cell_);
        read_page_table_link_cell_ = execution_context_->create_link_cell();
        execution_context_->link(read_page_table_link_cell_, 0);
        read_page_table_link_cell_address_ =
            execution_context_->link_cell_address(read_page_table_link_cell_);
    }

    ~JitExecutor() {
        execution_context_->unlink(runtime_link_cell_);
        execution_context_->unlink(lookup_link_cell_);
        execution_context_->unlink(runtime_config_link_cell_);
        execution_context_->unlink(page_table_link_cell_);
        execution_context_->unlink(read_page_table_link_cell_);
        performance_counters().record_jit_code_cache_usage(
            process_id_, static_cast<std::uint32_t>(execution_slot_),
            recorded_jit_code_cache_bytes_, 0);
        if (jit_) {
            performance_counters().record_jit_destroyed();
        }
    }

    CpuRunResult run(Cpu& cpu, std::uint64_t ticks, bool single_step) {
        const std::scoped_lock lock{execution_mutex_};
        memory_.synchronize_shared_write_tracking();
        ensure_jit();
        load_state(cpu);
        callbacks_->begin(single_step ? 1 : ticks);
        try {
            if (!single_step) {
                preload_current_artifact();
            }
            const auto reason = single_step ? jit_->Step() : jit_->Run();
            auto result = callbacks_->result(reason);
            save_state(cpu);
            record_code_cache_usage();
            performance_counters().record_cpu_execution(result.ticks_consumed);
            return result;
        } catch (...) {
            save_state(cpu);
            record_code_cache_usage();
            throw;
        }
    }

    void clear_cache() {
        artifact_probes_.clear();
        if (jit_) {
            jit_->ClearCache();
            record_code_cache_usage();
        }
    }

    void invalidate_cache_range(std::uint32_t address, std::size_t length) {
        if (jit_ && length != 0) {
            artifact_probes_.clear();
            jit_->InvalidateCacheRange(address, length);
            record_code_cache_usage();
        }
    }

    void clear_halt() {
        if (jit_) {
            jit_->ClearHalt(all_halt_reasons());
        }
    }

    void halt(Dynarmic::HaltReason reason) {
        if (jit_) {
            jit_->HaltExecution(reason);
        }
    }

    void clear_exclusive_state() {
        ensure_jit();
        jit_->ClearExclusiveState();
        monitor_.ClearProcessor(processor_id_);
    }

    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile,
        JitPrecompilePhase phase) {
        const auto locations =
            profile ? profile->snapshot() : std::vector<std::uint64_t>{};
        {
            const std::lock_guard execution_lock{execution_mutex_};
            callbacks_->set_translation_profile(profile);
        }
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        for (auto &queue : pending_precompile_entries_) queue.clear();
        pending_precompile_phases_.clear();
        inflight_precompile_entries_.clear();
        completed_precompile_entries_.clear();
        for (auto location = locations.rbegin(); location != locations.rend();
             ++location) {
            if (!enqueue_precompile_entry_locked(*location, phase)) break;
        }
    }

    void set_artifact_retention(JitArtifactRetention retention) {
        const std::lock_guard execution_lock{execution_mutex_};
        callbacks_->set_artifact_retention(retention);
    }

    void add_precompile_entries(
        const std::vector<std::uint64_t> &location_descriptors,
        JitPrecompilePhase phase) {
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        for (const auto entry : location_descriptors) {
            if (!enqueue_precompile_entry_locked(entry, phase)) break;
        }
    }

    [[nodiscard]] std::optional<JitPrecompilePhase>
    next_precompile_phase() {
        const std::lock_guard queue_lock{precompile_queue_mutex_};
        return next_precompile_phase_locked();
    }

    std::size_t precompile_pending(
        std::size_t maximum_blocks, std::uint64_t budget_nanoseconds,
        JitPrecompileTarget target) {
        if (maximum_blocks == 0 || budget_nanoseconds == 0) {
            return 0;
        }
        std::size_t candidates_remaining = 0;
        {
            const std::lock_guard queue_lock{precompile_queue_mutex_};
            if (pending_precompile_phases_.empty()) {
                return 0;
            }
            candidates_remaining = pending_precompile_phases_.size();
        }
        const std::lock_guard execution_lock{execution_mutex_};
        memory_.synchronize_shared_write_tracking();
        ensure_jit();
        constexpr std::size_t cache_reserve = 8U * 1024U * 1024U;
        const auto started = std::chrono::steady_clock::now();
        std::size_t compiled = 0;
        while (compiled < maximum_blocks && candidates_remaining != 0) {
            if (target == JitPrecompileTarget::NativeCode &&
                jit_code_cache_used(*jit_) + cache_reserve >= code_cache_size_) {
                break;
            }
            std::optional<std::pair<std::uint64_t, JitPrecompilePhase>> entry;
            {
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                entry = take_precompile_entry_locked();
            }
            if (!entry) break;
            --candidates_remaining;
            const auto descriptor = entry->first;
            const auto pc = static_cast<std::uint32_t>(descriptor);
            const auto code_address = pc & ~std::uint32_t{3};
            if (!memory_.accessible(
                    code_address, sizeof(std::uint32_t),
                    MemoryPermission::Execute)) {
                // A persisted profile can name a dylib before Guest dyld maps
                // it. Dynarmic would otherwise cache a NoExecute block which
                // remains stale after the mapping appears. Defer the hint
                // until its code page is executable instead.
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                inflight_precompile_entries_.erase(descriptor);
                static_cast<void>(enqueue_precompile_entry_locked(
                    descriptor, JitPrecompilePhase::Remaining));
            } else if (!memory_.translation_profile_stable(
                           code_address, sizeof(std::uint32_t))) {
                // Runtime slides can reuse a previously executable address for
                // a different image section. Never let such an advisory hint
                // turn unrelated data into a translated block.
                callbacks_->discard_translation_location(descriptor);
                const std::lock_guard queue_lock{precompile_queue_mutex_};
                inflight_precompile_entries_.erase(descriptor);
                completed_precompile_entries_.insert(descriptor);
            } else {
                const auto key = callbacks_->artifact_key(descriptor);
                const auto probe = key ? artifact_probes_.find(descriptor)
                                       : artifact_probes_.end();
                if (target == JitPrecompileTarget::NativeCode && key &&
                    probe != artifact_probes_.end() &&
                    probe->second.matches(*key)) {
                    const std::lock_guard queue_lock{precompile_queue_mutex_};
                    inflight_precompile_entries_.erase(descriptor);
                    completed_precompile_entries_.insert(descriptor);
                    ++compiled;
                    continue;
                }
                if (target == JitPrecompileTarget::PortableIr) {
                    auto available = callbacks_->artifact_available(descriptor);
                    if (!available) {
                        // The global offline worker fills the reusable IR
                        // store without consuming this process's native code
                        // cache. Generated bytes are round-trip validated by
                        // JitCallbacks before publication.
                        callbacks_->begin(0);
                        available = callbacks_->generate_portable_artifact(
                            *jit_, descriptor);
                    }
                    {
                        const std::lock_guard queue_lock{
                            precompile_queue_mutex_};
                        inflight_precompile_entries_.erase(descriptor);
                        completed_precompile_entries_.insert(descriptor);
                    }
                    if (available) ++compiled;
                } else {
                    const auto imported =
                        callbacks_->import_artifact(*jit_, descriptor);
                    if (!imported) {
                        const auto block_started =
                            std::chrono::steady_clock::now();
                        callbacks_->begin(0);
                        jit_->Precompile(descriptor);
                        performance_counters().record_jit_block_compile(
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<
                                    std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() -
                                    block_started)
                                    .count()));
                    }
                    if (key) {
                        artifact_probes_[descriptor] = ArtifactProbe{
                            key->content_identity, key->layout_identity,
                            imported};
                    }
                    {
                        const std::lock_guard queue_lock{
                            precompile_queue_mutex_};
                        inflight_precompile_entries_.erase(descriptor);
                        completed_precompile_entries_.insert(descriptor);
                    }
                    ++compiled;
                }
            }
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started);
            if (static_cast<std::uint64_t>(elapsed.count()) >=
                budget_nanoseconds) {
                break;
            }
        }
        record_code_cache_usage();
        return compiled;
    }

    void reset_live_state() {
        ensure_jit();
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
    struct ArtifactProbe {
        ContentIdentity content_identity;
        ContentIdentity layout_identity;
        bool imported{};

        [[nodiscard]] bool matches(const JitArtifactKey& key) const noexcept {
            return content_identity == key.content_identity &&
                   layout_identity == key.layout_identity;
        }
    };

    void preload_current_artifact() {
        const Dynarmic::A32::LocationDescriptor descriptor{
            jit_->Regs()[15], Dynarmic::A32::PSR{jit_->Cpsr()},
            Dynarmic::A32::FPSCR{jit_->Fpscr()}};
        const auto location =
            static_cast<Dynarmic::IR::LocationDescriptor>(descriptor).Value();
        if (artifact_probes_.find(location) != artifact_probes_.end()) {
            return;
        }
        const auto key = callbacks_->artifact_key(location);
        if (!key) return;
        const auto imported = callbacks_->import_artifact(*jit_, location);
        artifact_probes_.emplace(
            location,
            ArtifactProbe{key->content_identity, key->layout_identity,
                          imported});
    }

    [[nodiscard]] static constexpr Dynarmic::HaltReason all_halt_reasons() {
        return Dynarmic::HaltReason::MemoryAbort |
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
        config.callbacks_link = runtime_link_cell_address_;
        config.lookup_link = lookup_link_cell_address_;
        config.runtime_config_link = runtime_config_link_cell_address_;
        config.page_table_link = page_table_link_cell_address_;
        config.read_page_table_link = read_page_table_link_cell_address_;
        config.coprocessor_user_arg_link = runtime_link_cell_address_;
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
        performance_counters().record_jit_code_cache_usage(
            process_id_, static_cast<std::uint32_t>(execution_slot_),
            recorded_jit_code_cache_bytes_, current);
        recorded_jit_code_cache_bytes_ = current;
    }

    static constexpr std::size_t phase_index(JitPrecompilePhase phase) {
        return static_cast<std::size_t>(phase);
    }

    [[nodiscard]] bool enqueue_precompile_entry_locked(
        std::uint64_t entry, JitPrecompilePhase phase) {
        if (entry == 0 || completed_precompile_entries_.contains(entry) ||
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
                completed_precompile_entries_.size() >=
            jit_translation_profile_maximum_locations) {
            return false;
        }
        pending_precompile_phases_.emplace(entry, phase);
        pending_precompile_entries_[phase_index(phase)].push_back(entry);
        return true;
    }

    [[nodiscard]] std::optional<JitPrecompilePhase>
    next_precompile_phase_locked() {
        for (std::size_t index = 0; index < pending_precompile_entries_.size();
             ++index) {
            auto &queue = pending_precompile_entries_[index];
            while (!queue.empty()) {
                const auto entry = queue.front();
                const auto pending = pending_precompile_phases_.find(entry);
                if (pending != pending_precompile_phases_.end() &&
                    phase_index(pending->second) == index) {
                    return pending->second;
                }
                queue.pop_front();
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::pair<std::uint64_t,
                                          JitPrecompilePhase>>
    take_precompile_entry_locked() {
        const auto phase = next_precompile_phase_locked();
        if (!phase) return std::nullopt;
        auto &queue = pending_precompile_entries_[phase_index(*phase)];
        const auto entry = queue.front();
        queue.pop_front();
        pending_precompile_phases_.erase(entry);
        inflight_precompile_entries_.insert(entry);
        return std::pair{entry, *phase};
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
    std::size_t page_table_link_cell_{};
    std::atomic<std::uint64_t> *page_table_link_cell_address_{};
    std::size_t read_page_table_link_cell_{};
    std::atomic<std::uint64_t> *read_page_table_link_cell_address_{};
    std::unique_ptr<Dynarmic::A32::Jit> jit_;
    std::size_t code_cache_size_{64U * 1024U * 1024U};
    std::uint64_t recorded_jit_code_cache_bytes_{};
    std::array<std::deque<std::uint64_t>, jit_precompile_phase_count>
        pending_precompile_entries_;
    std::unordered_map<std::uint64_t, JitPrecompilePhase>
        pending_precompile_phases_;
    std::unordered_set<std::uint64_t> inflight_precompile_entries_;
    std::unordered_set<std::uint64_t> completed_precompile_entries_;
    std::unordered_map<std::uint64_t, ArtifactProbe> artifact_probes_;
    std::mutex precompile_queue_mutex_;
    std::mutex execution_mutex_;
};

class CpuExecutionPool {
public:
    CpuExecutionPool(
        AddressSpace& memory,
        Dynarmic::ExclusiveMonitor& monitor,
        std::size_t execution_slot_count,
        std::size_t first_processor_id,
        const ArmCpuModel& cpu_model,
        std::shared_ptr<JitArtifactStore> artifact_store)
        : memory_{memory},
          execution_context_{std::make_shared<ExecutionContext>()} {
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
        executors_.reserve(execution_slot_count);
        for (std::size_t slot = 0; slot < execution_slot_count; ++slot) {
            executors_.push_back(std::make_unique<JitExecutor>(
                first_processor_id + slot, slot, memory, monitor, cpu_model,
                artifact_store, execution_context_));
        }
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

    void clear_cache() {
        for (auto& executor : executors_) {
            executor->clear_cache();
        }
    }

    void invalidate_cache_range(std::uint32_t address, std::size_t length) {
        for (auto& executor : executors_) {
            executor->invalidate_cache_range(address, length);
        }
    }

    void disable_jit_page_table() {
        memory_.disable_jit_page_table();
    }

    void set_translation_profile(
        std::shared_ptr<JitTranslationProfile> profile,
        JitPrecompilePhase phase) {
        for (auto& executor : executors_) {
            executor->set_translation_profile(profile, phase);
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
        for (auto& executor : executors_) {
            executor->add_precompile_entries(location_descriptors, phase);
        }
    }

    [[nodiscard]] std::optional<JitPrecompilePhase>
    next_precompile_phase() {
        std::optional<JitPrecompilePhase> result;
        for (auto &executor : executors_) {
            const auto phase = executor->next_precompile_phase();
            if (phase &&
                (!result || static_cast<std::uint8_t>(*phase) <
                                static_cast<std::uint8_t>(*result))) {
                result = phase;
            }
        }
        return result;
    }

    std::size_t precompile_pending(
        std::size_t maximum_blocks, std::uint64_t budget_nanoseconds,
        JitPrecompileTarget target) {
        if (executors_.empty()) {
            return 0;
        }
        const auto blocks_per_executor =
            std::max<std::size_t>(1, maximum_blocks / executors_.size());
        const auto budget_per_executor =
            std::max<std::uint64_t>(1, budget_nanoseconds / executors_.size());
        std::size_t compiled = 0;
        for (auto& executor : executors_) {
            compiled += executor->precompile_pending(
                blocks_per_executor, budget_per_executor, target);
        }
        return compiled;
    }

private:
    AddressSpace& memory_;
    std::shared_ptr<ExecutionContext> execution_context_;
    std::vector<std::unique_ptr<JitExecutor>> executors_;
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
    return execution_pool_->executor(execution_slot).run(*this, ticks, false);
}

CpuRunResult Cpu::step(std::size_t execution_slot) {
    if (!execution_pool_) {
        throw std::logic_error{"CPU execution resources have been released"};
    }
    return execution_pool_->executor(execution_slot).run(*this, 1, true);
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

Dynarmic::HaltReason Cpu::consume_requested_halt_reason() {
    const auto reason = requested_halt_reason_;
    requested_halt_reason_ = {};
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

std::optional<JitPrecompilePhase> CpuCluster::next_precompile_phase() {
    if (!execution_pool_) return std::nullopt;
    return execution_pool_->next_precompile_phase();
}

std::size_t CpuCluster::precompile_pending(
    std::size_t maximum_blocks, std::uint64_t budget_nanoseconds,
    JitPrecompileTarget target) {
    if (!execution_pool_) {
        return 0;
    }
    return execution_pool_->precompile_pending(
        maximum_blocks, budget_nanoseconds, target);
}

std::shared_ptr<CpuExecutionPool>
CpuCluster::release_execution_resources() {
    if (!execution_pool_) {
        return {};
    }
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
