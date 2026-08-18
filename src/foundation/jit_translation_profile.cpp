#include "ilemu/jit_translation_profile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace ilemu {
namespace {

constexpr std::array<char, 8> profile_magic{
    'i', 'L', 'J', 'T', 'P', 'R', 'F', '3'};
constexpr std::uint32_t profile_schema_version = 3U;
constexpr std::array<char, 8> profile_index_magic{
    'i', 'L', 'J', 'T', 'I', 'D', 'X', '1'};
constexpr std::uint32_t profile_index_schema_version = 1U;
constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;
constexpr std::size_t profile_identity_bytes = sizeof(ContentIdentity{}.digest);
constexpr std::size_t profile_header_bytes =
    profile_magic.size() + sizeof(std::uint32_t) + sizeof(std::uint32_t) +
    sizeof(std::uint64_t) + profile_identity_bytes;

void hash_bytes(
    std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= fnv_prime;
    }
}

std::uint64_t profile_checksum(
    const ContentIdentity& identity,
    std::span<const std::uint64_t> locations) noexcept {
    auto hash = fnv_offset_basis;
    hash_bytes(hash, identity.digest.data(), identity.digest.size());
    for (const auto location : locations) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            const auto byte = static_cast<unsigned char>(location >> shift);
            hash_bytes(hash, &byte, 1);
        }
    }
    return hash;
}

std::string profile_file_stem(const ContentIdentity& identity) {
    return identity.hex();
}

std::filesystem::path profile_index_path(
    const std::filesystem::path& directory) {
    return directory / "profiles.index";
}

void write_u32(std::ostream& stream, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        stream.put(static_cast<char>(value >> shift));
    }
}

void write_u64(std::ostream& stream, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        stream.put(static_cast<char>(value >> shift));
    }
}

std::optional<std::uint32_t> read_u32(std::istream& stream) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        const auto byte = stream.get();
        if (byte == std::char_traits<char>::eof()) return std::nullopt;
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(byte))
                 << shift;
    }
    return value;
}

std::optional<std::uint64_t> read_u64(std::istream& stream) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
        const auto byte = stream.get();
        if (byte == std::char_traits<char>::eof()) return std::nullopt;
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(byte))
                 << shift;
    }
    return value;
}

struct ProfileLoadResult {
    std::vector<std::uint64_t> locations;
    std::uint64_t file_bytes{};
    bool accepted{};
};

ProfileLoadResult load_profile(
    const std::filesystem::path& path,
    const ContentIdentity& expected_identity) {
    std::error_code error;
    const auto file_bytes = std::filesystem::file_size(path, error);
    if (error || file_bytes < profile_header_bytes ||
        file_bytes > jit_translation_profile_maximum_file_bytes) {
        return {};
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) return {};

    std::array<char, profile_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    const auto schema = read_u32(stream);
    const auto location_count = read_u32(stream);
    const auto expected_checksum = read_u64(stream);
    ContentIdentity identity;
    stream.read(reinterpret_cast<char*>(identity.digest.data()),
                static_cast<std::streamsize>(identity.digest.size()));
    if (!stream || magic != profile_magic || !schema ||
        *schema != profile_schema_version || !location_count ||
        !expected_checksum || identity != expected_identity ||
        *location_count > jit_translation_profile_maximum_locations) {
        return {};
    }
    const auto expected_bytes =
        profile_header_bytes + static_cast<std::size_t>(*location_count) *
                                   sizeof(std::uint64_t);
    if (expected_bytes != file_bytes) return {};

    ProfileLoadResult result;
    result.locations.reserve(*location_count);
    for (std::uint32_t index = 0; index < *location_count; ++index) {
        const auto location = read_u64(stream);
        if (!location) return {};
        result.locations.push_back(*location);
    }
    if (stream.peek() != std::char_traits<char>::eof() ||
        profile_checksum(identity, result.locations) != *expected_checksum) {
        return {};
    }
    result.file_bytes = file_bytes;
    result.accepted = true;
    return result;
}

bool save_profile(
    const std::filesystem::path& directory,
    const ContentIdentity& identity,
    std::span<const std::uint64_t> locations) {
    if (identity.empty() || locations.empty() ||
        locations.size() > jit_translation_profile_maximum_locations) {
        return false;
    }
    const auto expected_bytes =
        profile_header_bytes + locations.size() * sizeof(std::uint64_t);
    if (expected_bytes > jit_translation_profile_maximum_file_bytes) {
        return false;
    }
    std::filesystem::create_directories(directory);
    const auto stem = profile_file_stem(identity);
    const auto target = directory / (stem + ".profile");
    const auto temporary = directory / (stem + ".profile.tmp");
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream) return false;
        stream.write(
            profile_magic.data(),
            static_cast<std::streamsize>(profile_magic.size()));
        write_u32(stream, profile_schema_version);
        write_u32(stream, static_cast<std::uint32_t>(locations.size()));
        write_u64(stream, profile_checksum(identity, locations));
        stream.write(reinterpret_cast<const char*>(identity.digest.data()),
                     static_cast<std::streamsize>(identity.digest.size()));
        for (const auto location : locations) write_u64(stream, location);
        stream.flush();
        if (!stream) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        error.clear();
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

struct ProfileIndexRecord {
    ContentIdentity identity;
    std::uint64_t access_order{};
    std::uint64_t file_bytes{};
};

std::vector<ProfileIndexRecord> load_profile_index(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto file_bytes = std::filesystem::file_size(path, error);
    constexpr std::size_t fixed_bytes =
        profile_index_magic.size() + sizeof(std::uint32_t) * 2U +
        sizeof(std::uint64_t);
    constexpr std::size_t record_bytes =
        profile_identity_bytes + sizeof(std::uint64_t) * 2U;
    if (error || file_bytes < fixed_bytes ||
        file_bytes > jit_translation_profile_maximum_file_bytes) {
        return {};
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) return {};
    std::array<char, profile_index_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    const auto schema = read_u32(stream);
    const auto count = read_u32(stream);
    const auto checksum = read_u64(stream);
    if (!stream || magic != profile_index_magic || !schema ||
        *schema != profile_index_schema_version || !count ||
        *count > jit_translation_profile_maximum_profiles || !checksum ||
        fixed_bytes + static_cast<std::size_t>(*count) * record_bytes !=
            file_bytes) {
        return {};
    }
    std::vector<std::byte> material;
    material.reserve(static_cast<std::size_t>(*count) * record_bytes);
    std::vector<ProfileIndexRecord> result;
    result.reserve(*count);
    auto append_u64_bytes = [&material](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            material.push_back(static_cast<std::byte>(value >> shift));
        }
    };
    for (std::uint32_t index = 0; index < *count; ++index) {
        ProfileIndexRecord record;
        stream.read(reinterpret_cast<char*>(record.identity.digest.data()),
                    static_cast<std::streamsize>(record.identity.digest.size()));
        const auto access_order = read_u64(stream);
        const auto bytes = read_u64(stream);
        if (!stream || !access_order || !bytes ||
            *bytes > jit_translation_profile_maximum_file_bytes) {
            return {};
        }
        record.access_order = *access_order;
        record.file_bytes = *bytes;
        material.insert(material.end(),
                        reinterpret_cast<const std::byte*>(
                            record.identity.digest.data()),
                        reinterpret_cast<const std::byte*>(
                            record.identity.digest.data()) +
                            record.identity.digest.size());
        append_u64_bytes(record.access_order);
        append_u64_bytes(record.file_bytes);
        result.push_back(record);
    }
    if (stream.peek() != std::char_traits<char>::eof()) return {};
    auto computed = fnv_offset_basis;
    hash_bytes(computed, material.data(), material.size());
    if (computed != *checksum) return {};
    return result;
}

bool save_profile_index(
    const std::filesystem::path& directory,
    const std::map<ContentIdentity, std::uint64_t>& access_order,
    const std::map<ContentIdentity, std::size_t>& file_bytes) {
    if (access_order.size() > jit_translation_profile_maximum_profiles) {
        return false;
    }
    std::vector<std::byte> material;
    material.reserve(access_order.size() *
                     (profile_identity_bytes + sizeof(std::uint64_t) * 2U));
    auto append_u64_bytes = [&material](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            material.push_back(static_cast<std::byte>(value >> shift));
        }
    };
    for (const auto& [identity, access] : access_order) {
        material.insert(material.end(),
                        reinterpret_cast<const std::byte*>(identity.digest.data()),
                        reinterpret_cast<const std::byte*>(identity.digest.data()) +
                            identity.digest.size());
        append_u64_bytes(access);
        const auto bytes = file_bytes.find(identity);
        append_u64_bytes(bytes == file_bytes.end() ? 0U : bytes->second);
    }
    auto checksum = fnv_offset_basis;
    hash_bytes(checksum, material.data(), material.size());

    std::filesystem::create_directories(directory);
    const auto target = profile_index_path(directory);
    const auto temporary = directory / "profiles.index.tmp";
    {
        std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
        if (!stream) return false;
        stream.write(profile_index_magic.data(),
                     static_cast<std::streamsize>(profile_index_magic.size()));
        write_u32(stream, profile_index_schema_version);
        write_u32(stream, static_cast<std::uint32_t>(access_order.size()));
        write_u64(stream, checksum);
        for (const auto& [identity, access] : access_order) {
            stream.write(reinterpret_cast<const char*>(identity.digest.data()),
                         static_cast<std::streamsize>(identity.digest.size()));
            write_u64(stream, access);
            const auto bytes = file_bytes.find(identity);
            write_u64(stream, bytes == file_bytes.end() ? 0U : bytes->second);
        }
        stream.flush();
        if (!stream) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        error.clear();
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

} // namespace

std::size_t JitTranslationProfileRecorder::hash(
    std::uint64_t location_descriptor) noexcept {
    auto value = location_descriptor;
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<std::size_t>(value);
}

JitTranslationProfileRecordResult JitTranslationProfileRecorder::record(
    std::uint64_t location_descriptor) noexcept {
    if (location_descriptor == 0) {
        return JitTranslationProfileRecordResult::Ignored;
    }
    auto slot = hash(location_descriptor) &
                (jit_translation_profile_recorder_hash_capacity - 1U);
    for (std::size_t probe = 0;
         probe < jit_translation_profile_recorder_hash_capacity; ++probe) {
        auto& known = known_locations_[slot];
        if (known == location_descriptor) {
            ++deduplicated_;
            return JitTranslationProfileRecordResult::Deduplicated;
        }
        if (known == 0) {
            if (size_ == jit_translation_profile_recorder_capacity) {
                ++dropped_capacity_;
                return JitTranslationProfileRecordResult::DroppedCapacity;
            }
            known = location_descriptor;
            locations_[size_++] = location_descriptor;
            return JitTranslationProfileRecordResult::Recorded;
        }
        slot = (slot + 1U) &
               (jit_translation_profile_recorder_hash_capacity - 1U);
    }
    ++dropped_capacity_;
    return JitTranslationProfileRecordResult::DroppedCapacity;
}

void JitTranslationProfileRecorder::reset() noexcept {
    known_locations_.fill(0);
    size_ = 0;
    deduplicated_ = 0;
    dropped_capacity_ = 0;
}

JitTranslationProfile::JitTranslationProfile(
    std::vector<std::uint64_t> location_descriptors) {
    const auto retained = std::min(
        location_descriptors.size(), jit_translation_profile_maximum_locations);
    known_locations_.reserve(retained * 2U);
    for (std::size_t index = 0; index < retained; ++index) {
        const auto location = location_descriptors[index];
        if (location != 0 && known_locations_.insert(location).second) {
            locations_.push_back(location);
            recorded_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (location_descriptors.size() > retained) {
        dropped_capacity_.fetch_add(
            location_descriptors.size() - retained, std::memory_order_relaxed);
    }
}

void JitTranslationProfile::record(
    std::uint64_t location_descriptor) noexcept {
    if (location_descriptor == 0) return;
    try {
        const std::lock_guard lock{mutex_};
        if (known_locations_.contains(location_descriptor) ||
            discarded_locations_.contains(location_descriptor)) {
            deduplicated_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (known_locations_.size() >=
            jit_translation_profile_maximum_locations) {
            dropped_capacity_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto [entry, inserted] =
            known_locations_.insert(location_descriptor);
        if (!inserted) {
            deduplicated_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        try {
            locations_.push_back(location_descriptor);
            recorded_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            known_locations_.erase(entry);
            dropped_capacity_.fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        dropped_capacity_.fetch_add(1, std::memory_order_relaxed);
    }
}

void JitTranslationProfile::merge(
    std::span<const std::uint64_t> location_descriptors,
    std::uint64_t recorder_deduplicated,
    std::uint64_t recorder_dropped_capacity) noexcept {
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t recorded = 0;
    std::uint64_t deduplicated = recorder_deduplicated;
    std::uint64_t dropped = recorder_dropped_capacity;
    try {
        const std::lock_guard lock{mutex_};
        for (const auto location_descriptor : location_descriptors) {
            if (location_descriptor == 0 ||
                known_locations_.contains(location_descriptor) ||
                discarded_locations_.contains(location_descriptor)) {
                ++deduplicated;
                continue;
            }
            if (known_locations_.size() >=
                jit_translation_profile_maximum_locations) {
                ++dropped;
                continue;
            }
            const auto [entry, inserted] =
                known_locations_.insert(location_descriptor);
            if (!inserted) {
                ++deduplicated;
                continue;
            }
            try {
                locations_.push_back(location_descriptor);
                ++recorded;
            } catch (...) {
                known_locations_.erase(entry);
                ++dropped;
            }
        }
        if (locations_.size() > jit_translation_profile_maximum_locations * 2U) {
            std::deque<std::uint64_t> compacted;
            compacted.clear();
            for (const auto location : locations_) {
                if (known_locations_.contains(location)) compacted.push_back(location);
            }
            locations_.swap(compacted);
            discarded_locations_.clear();
        }
    } catch (...) {
        dropped += location_descriptors.size();
    }
    recorded_.fetch_add(recorded, std::memory_order_relaxed);
    deduplicated_.fetch_add(deduplicated, std::memory_order_relaxed);
    dropped_capacity_.fetch_add(dropped, std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started);
    merge_calls_.fetch_add(1, std::memory_order_relaxed);
    merge_nanoseconds_.fetch_add(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed.count())),
        std::memory_order_relaxed);
}

void JitTranslationProfile::discard(
    std::uint64_t location_descriptor) noexcept {
    if (location_descriptor == 0) return;
    try {
        const std::lock_guard lock{mutex_};
        if (!known_locations_.contains(location_descriptor)) return;
        discarded_locations_.insert(location_descriptor);
        known_locations_.erase(location_descriptor);
    } catch (...) {
        // Invalidating a host optimization hint must not affect the guest.
    }
}

std::vector<std::uint64_t> JitTranslationProfile::snapshot() const {
    const std::lock_guard lock{mutex_};
    std::vector<std::uint64_t> result;
    result.reserve(known_locations_.size());
    for (const auto location : locations_) {
        if (known_locations_.contains(location)) result.push_back(location);
    }
    return result;
}

JitTranslationProfileStats JitTranslationProfile::stats() const noexcept {
    JitTranslationProfileStats result;
    result.recorded = recorded_.load(std::memory_order_relaxed);
    result.deduplicated = deduplicated_.load(std::memory_order_relaxed);
    result.dropped_capacity =
        dropped_capacity_.load(std::memory_order_relaxed);
    result.unstable_dropped = unstable_dropped_.load(std::memory_order_relaxed);
    result.profile_loaded = profile_loaded_.load(std::memory_order_relaxed);
    result.profile_files_loaded =
        profile_files_loaded_.load(std::memory_order_relaxed);
    result.profile_enqueued_portable =
        profile_enqueued_portable_.load(std::memory_order_relaxed);
    result.profile_portable_generated =
        profile_portable_generated_.load(std::memory_order_relaxed);
    result.portable_existence_hits =
        portable_existence_hits_.load(std::memory_order_relaxed);
    result.native_preimport_attempted =
        native_preimport_attempted_.load(std::memory_order_relaxed);
    result.native_preimport_imported =
        native_preimport_imported_.load(std::memory_order_relaxed);
    result.native_preimport_already_present =
        native_preimport_already_present_.load(std::memory_order_relaxed);
    result.native_preimport_before_first_demand =
        native_preimport_before_first_demand_.load(std::memory_order_relaxed);
    result.native_preimport_used =
        native_preimport_used_.load(std::memory_order_relaxed);
    result.demand_artifact_staged =
        demand_artifact_staged_.load(std::memory_order_relaxed);
    result.demand_artifact_consumed =
        demand_artifact_consumed_.load(std::memory_order_relaxed);
    result.demand_artifact_stage_unused =
        demand_artifact_stage_unused_.load(std::memory_order_relaxed);
    result.profile_imported_before_first_run =
        profile_imported_before_first_run_.load(std::memory_order_relaxed);
    result.merge_calls = merge_calls_.load(std::memory_order_relaxed);
    result.merge_nanoseconds =
        merge_nanoseconds_.load(std::memory_order_relaxed);
    result.save_calls = save_calls_.load(std::memory_order_relaxed);
    result.save_nanoseconds =
        save_nanoseconds_.load(std::memory_order_relaxed);
    result.load_nanoseconds = load_nanoseconds_.load(std::memory_order_relaxed);
    result.profile_bytes = profile_bytes_.load(std::memory_order_relaxed);
    result.profile_save_failures =
        profile_save_failures_.load(std::memory_order_relaxed);
    {
        const std::lock_guard lock{mutex_};
        result.resident_bytes = locations_.size() * sizeof(std::uint64_t) +
                                known_locations_.size() * sizeof(std::uint64_t) *
                                    2U +
                                discarded_locations_.size() *
                                    sizeof(std::uint64_t) * 2U;
    }
    return result;
}

void JitTranslationProfile::note_profile_loaded(
    std::uint64_t descriptors) noexcept {
    profile_loaded_.fetch_add(descriptors, std::memory_order_relaxed);
    if (descriptors != 0) {
        profile_files_loaded_.fetch_add(1, std::memory_order_relaxed);
    }
}
void JitTranslationProfile::note_profile_enqueued_portable(
    std::uint64_t count) noexcept {
    profile_enqueued_portable_.fetch_add(count, std::memory_order_relaxed);
}
void JitTranslationProfile::note_portable_existence_hit() noexcept {
    portable_existence_hits_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_profile_portable_generated() noexcept {
    profile_portable_generated_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_native_preimport_attempted(
    bool before_first_demand) noexcept {
    native_preimport_attempted_.fetch_add(1, std::memory_order_relaxed);
    if (before_first_demand) {
        native_preimport_before_first_demand_.fetch_add(
            1, std::memory_order_relaxed);
        profile_imported_before_first_run_.fetch_add(
            1, std::memory_order_relaxed);
    }
}
void JitTranslationProfile::note_native_preimport_imported() noexcept {
    native_preimport_imported_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_native_preimport_already_present() noexcept {
    native_preimport_already_present_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_native_preimport_used() noexcept {
    native_preimport_used_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_demand_artifact_staged() noexcept {
    demand_artifact_staged_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_demand_artifact_consumed() noexcept {
    demand_artifact_consumed_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_demand_artifact_stage_unused() noexcept {
    demand_artifact_stage_unused_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_profile_imported_before_first_run() noexcept {
    profile_imported_before_first_run_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_unstable_dropped(std::uint64_t count) noexcept {
    unstable_dropped_.fetch_add(count, std::memory_order_relaxed);
}
void JitTranslationProfile::note_save(
    std::uint64_t nanoseconds, std::uint64_t bytes) noexcept {
    save_calls_.fetch_add(1, std::memory_order_relaxed);
    save_nanoseconds_.fetch_add(nanoseconds, std::memory_order_relaxed);
    profile_bytes_.store(bytes, std::memory_order_relaxed);
}
void JitTranslationProfile::note_save_failure() noexcept {
    profile_save_failures_.fetch_add(1, std::memory_order_relaxed);
}
void JitTranslationProfile::note_profile_bytes(std::uint64_t bytes) noexcept {
    profile_bytes_.store(bytes, std::memory_order_relaxed);
}
void JitTranslationProfile::note_load(std::uint64_t nanoseconds) noexcept {
    load_nanoseconds_.fetch_add(nanoseconds, std::memory_order_relaxed);
}

JitTranslationProfileStore::JitTranslationProfileStore(
    std::filesystem::path data_directory)
    : data_directory_{std::move(data_directory)} {
    for (const auto& record : load_profile_index(
             profile_index_path(data_directory_))) {
        profile_access_order_[record.identity] = record.access_order;
        known_profile_bytes_[record.identity] =
            static_cast<std::size_t>(record.file_bytes);
        known_storage_bytes_ += static_cast<std::size_t>(record.file_bytes);
        next_access_order_ = std::max(next_access_order_, record.access_order + 1U);
    }
}

JitTranslationProfileStore::~JitTranslationProfileStore() { save(); }

std::shared_ptr<JitTranslationProfile>
JitTranslationProfileStore::profile_for(
    const ContentIdentity& executable_identity) {
    if (const auto entry = profiles_.find(executable_identity);
        entry != profiles_.end()) {
        profile_access_order_[executable_identity] = next_access_order_++;
        return entry->second;
    }

    // A process can encounter more images than the persistent profile budget.
    // Keep execution working with an ephemeral profile instead of allowing the
    // profile map or its save set to grow without bound.
    if (profiles_.size() >= jit_translation_profile_maximum_profiles) {
        return std::make_shared<JitTranslationProfile>();
    }

    std::uint64_t loaded_bytes{};
    if (!executable_identity.empty()) {
        const auto path = data_directory_ /
                          (profile_file_stem(executable_identity) + ".profile");
        const auto started = std::chrono::steady_clock::now();
        const auto loaded = load_profile(path, executable_identity);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        auto profile = std::make_shared<JitTranslationProfile>(
            loaded.locations);
        profile->note_load(static_cast<std::uint64_t>(std::max<std::int64_t>(
            0, elapsed.count())));
        if (loaded.accepted) {
            loaded_bytes = loaded.file_bytes;
            profile->note_profile_loaded(loaded.locations.size());
            profile->note_profile_bytes(loaded.file_bytes);
            ++profile_loads_;
        }
        profiles_.emplace(executable_identity, std::move(profile));
        profile_access_order_[executable_identity] = next_access_order_++;
        if (loaded_bytes != 0) {
            known_profile_bytes_[executable_identity] =
                static_cast<std::size_t>(loaded_bytes);
            known_storage_bytes_ += static_cast<std::size_t>(loaded_bytes);
        }
        return profiles_.at(executable_identity);
    }
    auto profile = std::make_shared<JitTranslationProfile>();
    profiles_.emplace(executable_identity, profile);
    profile_access_order_[executable_identity] = next_access_order_++;
    return profile;
}

void JitTranslationProfileStore::save() noexcept {
    try {
        for (const auto& [executable_identity, profile] : profiles_) {
            if (!profile || executable_identity.empty()) continue;
            const auto locations = profile->snapshot();
            if (locations.empty()) continue;
            const auto started = std::chrono::steady_clock::now();
            const bool saved = save_profile(
                data_directory_, executable_identity, locations);
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started);
            std::error_code error;
            const auto file_bytes = saved
                ? std::filesystem::file_size(
                      data_directory_ /
                          (profile_file_stem(executable_identity) + ".profile"),
                      error)
                : 0U;
            profile->note_save(
                static_cast<std::uint64_t>(std::max<std::int64_t>(
                    0, elapsed.count())),
                error ? 0U : file_bytes);
            if (!saved || error) {
                profile->note_save_failure();
                ++profile_save_failures_;
                continue;
            }
            const auto previous = known_profile_bytes_.find(executable_identity);
            if (previous != known_profile_bytes_.end()) {
                known_storage_bytes_ =
                    previous->second > known_storage_bytes_
                        ? 0U
                        : known_storage_bytes_ - previous->second;
            }
            known_profile_bytes_[executable_identity] =
                static_cast<std::size_t>(file_bytes);
            known_storage_bytes_ += static_cast<std::size_t>(file_bytes);
        }

        while (known_storage_bytes_ >
                   jit_translation_profile_maximum_storage_bytes &&
               !known_profile_bytes_.empty()) {
            auto oldest = known_profile_bytes_.begin();
            for (auto candidate = std::next(known_profile_bytes_.begin());
                 candidate != known_profile_bytes_.end(); ++candidate) {
                if (profile_access_order_[candidate->first] <
                    profile_access_order_[oldest->first]) {
                    oldest = candidate;
                }
            }
            const auto identity = oldest->first;
            const auto path = data_directory_ /
                              (profile_file_stem(identity) + ".profile");
            std::error_code error;
            std::filesystem::remove(path, error);
            known_storage_bytes_ =
                oldest->second > known_storage_bytes_
                    ? 0U
                    : known_storage_bytes_ - oldest->second;
            known_profile_bytes_.erase(oldest);
            profile_access_order_.erase(identity);
            profiles_.erase(identity);
        }
        static_cast<void>(save_profile_index(
            data_directory_, profile_access_order_, known_profile_bytes_));
    } catch (...) {
        // A cache write must not change simulator shutdown semantics.
    }
}

JitTranslationProfileStats JitTranslationProfileStore::stats() const noexcept {
    JitTranslationProfileStats result;
    result.profile_bytes = known_storage_bytes_;
    for (const auto& [identity, profile] : profiles_) {
        static_cast<void>(identity);
        if (!profile) continue;
        const auto current = profile->stats();
        result.recorded += current.recorded;
        result.deduplicated += current.deduplicated;
        result.dropped_capacity += current.dropped_capacity;
        result.unstable_dropped += current.unstable_dropped;
        result.profile_loaded += current.profile_loaded;
        result.profile_files_loaded += current.profile_files_loaded;
        result.profile_enqueued_portable += current.profile_enqueued_portable;
        result.profile_portable_generated += current.profile_portable_generated;
        result.portable_existence_hits += current.portable_existence_hits;
        result.native_preimport_attempted += current.native_preimport_attempted;
        result.native_preimport_imported += current.native_preimport_imported;
        result.native_preimport_already_present +=
            current.native_preimport_already_present;
        result.native_preimport_before_first_demand +=
            current.native_preimport_before_first_demand;
        result.native_preimport_used += current.native_preimport_used;
        result.demand_artifact_staged += current.demand_artifact_staged;
        result.demand_artifact_consumed += current.demand_artifact_consumed;
        result.demand_artifact_stage_unused +=
            current.demand_artifact_stage_unused;
        result.profile_imported_before_first_run +=
            current.profile_imported_before_first_run;
        result.merge_calls += current.merge_calls;
        result.merge_nanoseconds += current.merge_nanoseconds;
        result.save_calls += current.save_calls;
        result.save_nanoseconds += current.save_nanoseconds;
        result.load_nanoseconds += current.load_nanoseconds;
        result.profile_save_failures += current.profile_save_failures;
        result.resident_bytes += current.resident_bytes;
    }
    return result;
}

} // namespace ilemu
