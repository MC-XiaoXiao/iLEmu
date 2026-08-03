#include "ilemu/jit_translation_profile.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace ilemu {
namespace {

constexpr std::array<char, 8> profile_magic{
    'i', 'L', 'J', 'T', 'P', 'R', 'F', '1'};
constexpr std::size_t maximum_profile_path_bytes = 4096;
constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

void hash_bytes(
    std::uint64_t& hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= fnv_prime;
    }
}

std::uint64_t profile_checksum(
    std::string_view executable_path,
    std::span<const std::uint64_t> locations) noexcept {
    auto hash = fnv_offset_basis;
    hash_bytes(hash, executable_path.data(), executable_path.size());
    for (const auto location : locations) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            const auto byte = static_cast<unsigned char>(location >> shift);
            hash_bytes(hash, &byte, 1);
        }
    }
    return hash;
}

std::string profile_file_stem(std::string_view executable_path) {
    auto hash = fnv_offset_basis;
    hash_bytes(hash, executable_path.data(), executable_path.size());
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
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
        if (byte == std::char_traits<char>::eof()) {
            return std::nullopt;
        }
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
        if (byte == std::char_traits<char>::eof()) {
            return std::nullopt;
        }
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(byte))
                 << shift;
    }
    return value;
}

std::vector<std::uint64_t> load_profile(
    const std::filesystem::path& path,
    std::string_view expected_executable_path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return {};
    }
    std::array<char, profile_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != profile_magic) {
        return {};
    }
    const auto path_size = read_u32(stream);
    const auto location_count = read_u32(stream);
    const auto expected_checksum = read_u64(stream);
    if (!path_size || !location_count || !expected_checksum ||
        *path_size > maximum_profile_path_bytes ||
        *location_count > jit_translation_profile_maximum_locations) {
        return {};
    }
    std::string executable_path(*path_size, '\0');
    stream.read(
        executable_path.data(),
        static_cast<std::streamsize>(executable_path.size()));
    if (!stream || executable_path != expected_executable_path) {
        return {};
    }
    std::vector<std::uint64_t> locations;
    locations.reserve(*location_count);
    for (std::uint32_t index = 0; index < *location_count; ++index) {
        const auto location = read_u64(stream);
        if (!location) {
            return {};
        }
        locations.push_back(*location);
    }
    if (stream.peek() != std::char_traits<char>::eof() ||
        profile_checksum(executable_path, locations) != *expected_checksum) {
        return {};
    }
    return locations;
}

void save_profile(
    const std::filesystem::path& directory,
    std::string_view executable_path,
    std::span<const std::uint64_t> locations) {
    if (executable_path.empty() ||
        executable_path.size() > maximum_profile_path_bytes ||
        locations.empty() ||
        locations.size() > jit_translation_profile_maximum_locations) {
        return;
    }
    std::filesystem::create_directories(directory);
    const auto stem = profile_file_stem(executable_path);
    const auto target = directory / (stem + ".profile");
    const auto temporary = directory / (stem + ".profile.tmp");
    {
        std::ofstream stream{
            temporary, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return;
        }
        stream.write(
            profile_magic.data(),
            static_cast<std::streamsize>(profile_magic.size()));
        write_u32(
            stream, static_cast<std::uint32_t>(executable_path.size()));
        write_u32(stream, static_cast<std::uint32_t>(locations.size()));
        write_u64(
            stream, profile_checksum(executable_path, locations));
        stream.write(
            executable_path.data(),
            static_cast<std::streamsize>(executable_path.size()));
        for (const auto location : locations) {
            write_u64(stream, location);
        }
        stream.flush();
        if (!stream) {
            return;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        error.clear();
        std::filesystem::remove(temporary, error);
    }
}

} // namespace

JitTranslationProfile::JitTranslationProfile(
    std::vector<std::uint64_t> location_descriptors) {
    const auto retained =
        std::min(location_descriptors.size(),
                 jit_translation_profile_maximum_locations);
    known_locations_.reserve(retained);
    const auto first =
        location_descriptors.end() -
        static_cast<std::ptrdiff_t>(retained);
    for (auto location = first; location != location_descriptors.end();
         ++location) {
        if (*location != 0 &&
            known_locations_.insert(*location).second) {
            locations_.push_back(*location);
        }
    }
}

void JitTranslationProfile::record(
    std::uint64_t location_descriptor) noexcept {
    if (location_descriptor == 0)
        return;
    try {
        const std::lock_guard lock{mutex_};
        if (known_locations_.contains(location_descriptor) ||
            discarded_locations_.contains(location_descriptor)) {
            return;
        }
        if (locations_.size() ==
            jit_translation_profile_maximum_locations) {
            const auto oldest = locations_.front();
            locations_.pop_front();
            known_locations_.erase(oldest);
            discarded_locations_.erase(oldest);
        }
        const auto [entry, inserted] =
            known_locations_.insert(location_descriptor);
        if (inserted) {
            try {
                locations_.push_back(location_descriptor);
            } catch (...) {
                known_locations_.erase(entry);
                throw;
            }
        }
    } catch (...) {
        // Recording is advisory. Translation and guest execution remain the
        // source of truth when the host cannot grow the profile.
    }
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

JitTranslationProfileStore::JitTranslationProfileStore(
    std::filesystem::path data_directory)
    : data_directory_{std::move(data_directory)} {}

JitTranslationProfileStore::~JitTranslationProfileStore() {
    save();
}

std::shared_ptr<JitTranslationProfile>
JitTranslationProfileStore::profile_for(
    std::string_view executable_path) {
    auto [entry, inserted] = profiles_.try_emplace(
        std::string{executable_path});
    if (inserted) {
        const auto path =
            data_directory_ /
            (profile_file_stem(executable_path) + ".profile");
        entry->second = std::make_shared<JitTranslationProfile>(
            load_profile(path, executable_path));
    }
    return entry->second;
}

void JitTranslationProfileStore::save() noexcept {
    try {
        for (const auto& [executable_path, profile] : profiles_) {
            if (!profile) {
                continue;
            }
            save_profile(
                data_directory_, executable_path, profile->snapshot());
        }
    } catch (...) {
        // A cache write must not change simulator shutdown semantics.
    }
}

} // namespace ilemu
