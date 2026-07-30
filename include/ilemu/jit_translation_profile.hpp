#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace ilemu {

inline constexpr std::size_t
    jit_translation_profile_maximum_locations = 131'072;

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
    [[nodiscard]] std::vector<std::uint64_t> snapshot() const;

private:
    mutable std::mutex mutex_;
    std::deque<std::uint64_t> locations_;
    std::unordered_set<std::uint64_t> known_locations_;
};

// Profiles are host cache hints stored outside the guest root filesystem.
// Files contain descriptors, never generated host machine code. Invalid,
// stale, or truncated files are ignored and replaced after a clean simulator
// shutdown.
class JitTranslationProfileStore {
public:
    explicit JitTranslationProfileStore(
        std::filesystem::path data_directory);
    ~JitTranslationProfileStore();

    JitTranslationProfileStore(const JitTranslationProfileStore&) = delete;
    JitTranslationProfileStore& operator=(
        const JitTranslationProfileStore&) = delete;

    [[nodiscard]] std::shared_ptr<JitTranslationProfile> profile_for(
        std::string_view executable_path);
    void save() noexcept;

private:
    std::filesystem::path data_directory_;
    std::map<std::string, std::shared_ptr<JitTranslationProfile>> profiles_;
};

} // namespace ilemu
