#include "ilemu/jit_artifact.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <sys/file.h>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <utility>

namespace ilemu {
namespace {

constexpr std::array<char, 8> artifact_magic{
    'i', 'L', 'J', 'A', 'R', 'T', 'F', '7'};
constexpr std::array<char, 8> artifact_append_magic{
    'i', 'L', 'J', 'A', 'P', 'P', 'F', '2'};
constexpr std::array<char, 8> artifact_index_magic{
    'i', 'L', 'J', 'I', 'D', 'X', 'F', '1'};
constexpr std::array<char, 8> artifact_footer_magic{
    'i', 'L', 'J', 'F', 'O', 'O', 'T', '1'};
constexpr std::size_t artifact_checksum_bytes = 32U;
constexpr std::size_t serialized_key_bytes = 132U;
constexpr std::size_t artifact_header_bytes =
    artifact_magic.size() + sizeof(std::uint32_t);
constexpr std::size_t artifact_index_entry_bytes =
    serialized_key_bytes + sizeof(std::uint64_t) * 2U +
    artifact_checksum_bytes;
constexpr std::size_t artifact_index_header_bytes =
    artifact_index_magic.size() + sizeof(std::uint32_t);
constexpr std::size_t artifact_footer_bytes =
    artifact_footer_magic.size() + sizeof(std::uint64_t) * 2U +
    sizeof(std::uint32_t) + artifact_checksum_bytes;
constexpr std::uint32_t maximum_artifacts = 1'000'000;
constexpr std::uint32_t maximum_ir_bytes = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_metadata_entries = 1'000'000;
constexpr std::uintmax_t maximum_persistence_bytes =
    std::uintmax_t{4U} * 1024U * 1024U * 1024U;
std::atomic<std::uint64_t> next_context_id{1};

class ArtifactFileLock {
public:
  enum class Mode { Shared, Exclusive };

  ArtifactFileLock(const ArtifactFileLock &) = delete;
  ArtifactFileLock &operator=(const ArtifactFileLock &) = delete;
  ArtifactFileLock(ArtifactFileLock &&other) noexcept
      : descriptor_{std::exchange(other.descriptor_, -1)} {}
  ArtifactFileLock &operator=(ArtifactFileLock &&) = delete;
  ~ArtifactFileLock() {
    if (descriptor_ < 0) return;
    static_cast<void>(::flock(descriptor_, LOCK_UN));
    static_cast<void>(::close(descriptor_));
  }

  [[nodiscard]] static std::optional<ArtifactFileLock>
  acquire(const std::filesystem::path &persistence_path, Mode mode) noexcept {
    try {
      const auto lock_path = std::filesystem::path{
          persistence_path.string() + ".writer.lock"};
      const auto parent = lock_path.parent_path();
      if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) return std::nullopt;
      }
      const auto descriptor =
          ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
      if (descriptor < 0) return std::nullopt;
      const auto operation = mode == Mode::Exclusive ? LOCK_EX : LOCK_SH;
      if (::flock(descriptor, operation) != 0) {
        static_cast<void>(::close(descriptor));
        return std::nullopt;
      }
      return ArtifactFileLock{descriptor};
    } catch (...) {
      return std::nullopt;
    }
  }

  [[nodiscard]] std::optional<std::uint64_t> generation() const noexcept {
    std::array<std::byte, sizeof(std::uint64_t)> bytes{};
    const auto count = ::pread(descriptor_, bytes.data(), bytes.size(), 0);
    if (count == 0) return 0U;
    if (count != static_cast<ssize_t>(bytes.size())) return std::nullopt;
    std::uint64_t result = 0;
    for (unsigned index = 0; index < bytes.size(); ++index) {
      result |= static_cast<std::uint64_t>(
                    std::to_integer<std::uint8_t>(bytes[index]))
                << (index * 8U);
    }
    return result;
  }

  [[nodiscard]] std::optional<std::uint64_t> begin_write() const noexcept {
    const auto current = generation();
    if (!current || *current == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
    }
    const auto next = *current + 1U;
    std::array<std::byte, sizeof(next)> bytes{};
    for (unsigned index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<std::byte>(next >> (index * 8U));
    }
    if (::pwrite(descriptor_, bytes.data(), bytes.size(), 0) !=
            static_cast<ssize_t>(bytes.size()) ||
        ::ftruncate(descriptor_, static_cast<off_t>(bytes.size())) != 0 ||
        ::fsync(descriptor_) != 0) {
      return std::nullopt;
    }
    return next;
  }

private:
  explicit ArtifactFileLock(int descriptor) : descriptor_{descriptor} {}
  int descriptor_{-1};
};

class ArtifactReadHandle {
public:
  ArtifactReadHandle(const ArtifactReadHandle &) = delete;
  ArtifactReadHandle &operator=(const ArtifactReadHandle &) = delete;
  ArtifactReadHandle(ArtifactReadHandle &&other) noexcept
      : descriptor_{std::exchange(other.descriptor_, -1)} {}
  ArtifactReadHandle &operator=(ArtifactReadHandle &&) = delete;
  ~ArtifactReadHandle() {
    if (descriptor_ >= 0) static_cast<void>(::close(descriptor_));
  }

  [[nodiscard]] static std::optional<ArtifactReadHandle>
  open(const std::filesystem::path &path) noexcept {
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return std::nullopt;
    return ArtifactReadHandle{descriptor};
  }

  [[nodiscard]] int descriptor() const noexcept { return descriptor_; }

  [[nodiscard]] std::ifstream stream() const {
    // Reopening the descriptor link retains the inode selected above even if
    // another process atomically replaces the cache path between checksum and
    // deserialization.
    std::ifstream result{
        std::filesystem::path{"/proc/self/fd/" +
                              std::to_string(descriptor_)},
        std::ios::binary};
    if (result) return result;
    return std::ifstream{
        std::filesystem::path{"/dev/fd/" + std::to_string(descriptor_)},
        std::ios::binary};
  }

private:
  explicit ArtifactReadHandle(int descriptor) : descriptor_{descriptor} {}
  int descriptor_{-1};
};

void hash_bytes(std::size_t &hash, const void *data, std::size_t size) noexcept {
  const auto *bytes = static_cast<const std::byte *>(data);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= std::to_integer<std::uint8_t>(bytes[index]) +
            static_cast<std::size_t>(0x9e3779b9U) + (hash << 6U) +
            (hash >> 2U);
  }
}

template <typename T>
void hash_scalar(std::size_t &hash, T value) noexcept {
  hash_bytes(hash, &value, sizeof(value));
}

void hash_identity(std::size_t &hash, const ContentIdentity &identity) noexcept {
  hash_bytes(hash, identity.digest.data(), identity.digest.size());
}

void write_u32(std::ostream &stream, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    stream.put(static_cast<char>(value >> shift));
  }
}

void write_u64(std::ostream &stream, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    stream.put(static_cast<char>(value >> shift));
  }
}

[[nodiscard]] std::optional<std::uint32_t> read_u32(std::istream &stream) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32U; shift += 8U) {
    const auto byte = stream.get();
    if (byte == std::char_traits<char>::eof()) return std::nullopt;
    value |= static_cast<std::uint32_t>(static_cast<unsigned char>(byte))
             << shift;
  }
  return value;
}

[[nodiscard]] std::optional<std::uint64_t> read_u64(std::istream &stream) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64U; shift += 8U) {
    const auto byte = stream.get();
    if (byte == std::char_traits<char>::eof()) return std::nullopt;
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte))
             << shift;
  }
  return value;
}

void write_identity(std::ostream &stream, const ContentIdentity &identity) {
  stream.write(reinterpret_cast<const char *>(identity.digest.data()),
               static_cast<std::streamsize>(identity.digest.size()));
}

[[nodiscard]] bool read_identity(std::istream &stream,
                                 ContentIdentity &identity) {
  stream.read(reinterpret_cast<char *>(identity.digest.data()),
              static_cast<std::streamsize>(identity.digest.size()));
  return static_cast<bool>(stream);
}

void write_key(std::ostream &stream, const JitArtifactKey &key) {
  write_identity(stream, key.content_identity);
  write_identity(stream, key.layout_identity);
  write_u32(stream, key.guest_pc);
  stream.put(static_cast<char>(key.thumb));
  write_u64(stream, key.location_descriptor);
  stream.put(static_cast<char>(key.architecture));
  stream.put(static_cast<char>(key.cpu_model));
  stream.put('\0');
  write_u32(stream, key.timing_model_version);
  write_u32(stream, key.guest_ticks_per_second);
  write_u32(stream, key.image_slide);
  write_u32(stream, key.hle_abi_version);
  write_u32(stream, key.backend_abi_version);
  write_u64(stream, key.dynarmic_build_fingerprint);
  write_u64(stream, key.codegen_options);
  stream.put(static_cast<char>(key.host_isa));
  stream.put('\0');
  stream.put('\0');
  stream.put('\0');
  write_u64(stream, key.host_feature_mask);
  write_u32(stream, key.artifact_format_version);
}

[[nodiscard]] std::optional<JitArtifactKey> read_key(std::istream &stream) {
  JitArtifactKey key;
  if (!read_identity(stream, key.content_identity) ||
      !read_identity(stream, key.layout_identity)) {
    return std::nullopt;
  }
  const auto guest_pc = read_u32(stream);
  if (!guest_pc) return std::nullopt;
  key.guest_pc = *guest_pc;
  const auto thumb = stream.get();
  if (thumb == std::char_traits<char>::eof()) return std::nullopt;
  key.thumb = thumb != 0;
  const auto location_descriptor = read_u64(stream);
  if (!location_descriptor) return std::nullopt;
  key.location_descriptor = *location_descriptor;
  const auto architecture = stream.get();
  const auto cpu_model = stream.get();
  if (thumb == std::char_traits<char>::eof() ||
      architecture == std::char_traits<char>::eof() ||
      cpu_model == std::char_traits<char>::eof() ||
      stream.get() == std::char_traits<char>::eof()) {
    return std::nullopt;
  }
  key.architecture = static_cast<ArmArchitectureVersion>(architecture);
  key.cpu_model = static_cast<ArmCpuModelKind>(cpu_model);
  const auto timing = read_u32(stream);
  const auto ticks = read_u32(stream);
  const auto slide = read_u32(stream);
  const auto hle = read_u32(stream);
  const auto backend = read_u32(stream);
  const auto dynarmic = read_u64(stream);
  const auto options = read_u64(stream);
  if (!timing || !ticks || !slide || !hle || !backend || !dynarmic ||
      !options) {
    return std::nullopt;
  }
  key.timing_model_version = *timing;
  key.guest_ticks_per_second = *ticks;
  key.image_slide = *slide;
  key.hle_abi_version = *hle;
  key.backend_abi_version = *backend;
  key.dynarmic_build_fingerprint = *dynarmic;
  key.codegen_options = *options;
  const auto host_isa = stream.get();
  if (host_isa == std::char_traits<char>::eof() ||
      stream.get() == std::char_traits<char>::eof() ||
      stream.get() == std::char_traits<char>::eof() ||
      stream.get() == std::char_traits<char>::eof()) {
    return std::nullopt;
  }
  key.host_isa = static_cast<JitHostIsa>(host_isa);
  const auto host_features = read_u64(stream);
  const auto format = read_u32(stream);
  if (!host_features || !format) return std::nullopt;
  key.host_feature_mask = *host_features;
  key.artifact_format_version = *format;
  if (key.architecture > ArmArchitectureVersion::Armv7 ||
      key.cpu_model > ArmCpuModelKind::CortexA8 ||
      key.host_isa > JitHostIsa::Arm64) {
    return std::nullopt;
  }
  return key;
}

[[nodiscard]] std::optional<std::vector<std::byte>>
read_bytes(std::istream &stream, std::uint32_t count) {
  if (count > maximum_ir_bytes) return std::nullopt;
  std::vector<std::byte> bytes(count);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream) return std::nullopt;
  return bytes;
}

[[nodiscard]] std::optional<std::size_t> serialized_artifact_bytes(
    std::size_t normalized_ir_bytes, std::size_t relocation_count,
    std::size_t exit_count, std::size_t dependency_count,
    std::size_t constant_dependency_count) {
  if (normalized_ir_bytes > maximum_ir_bytes ||
      relocation_count > maximum_metadata_entries ||
      exit_count > maximum_metadata_entries) {
    return std::nullopt;
  }
  constexpr std::size_t serialized_data_header_bytes = 32U;
  constexpr std::size_t serialized_dependency_bytes = 72U;
  constexpr std::size_t serialized_constant_dependency_bytes = 80U;
  if (dependency_count > maximum_metadata_entries ||
      dependency_count >
          std::numeric_limits<std::size_t>::max() /
              serialized_dependency_bytes ||
      constant_dependency_count > maximum_metadata_entries ||
      constant_dependency_count >
          std::numeric_limits<std::size_t>::max() /
              serialized_constant_dependency_bytes) {
    return std::nullopt;
  }
  std::size_t size = serialized_key_bytes + serialized_data_header_bytes;
  const auto add = [&size](std::size_t value) {
    if (value > std::numeric_limits<std::size_t>::max() - size) {
      return false;
    }
    size += value;
    return true;
  };
  return add(normalized_ir_bytes) &&
                 add(relocation_count * sizeof(std::uint64_t)) &&
                 add(exit_count * sizeof(std::uint64_t)) &&
                 add(dependency_count *
                     serialized_dependency_bytes) &&
                 add(constant_dependency_count *
                     serialized_constant_dependency_bytes)
             ? std::optional<std::size_t>{size}
             : std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> serialized_artifact_bytes(
    const JitArtifactData &data) {
  return serialized_artifact_bytes(
      data.normalized_ir.size(), data.relocation_targets.size(),
      data.exit_locations.size(), data.code_dependencies.size(),
      data.constant_dependencies.size());
}

[[nodiscard]] bool skip_bytes(std::istream &stream, std::uint64_t count,
                              std::uint64_t payload_size) {
  const auto current = stream.tellg();
  if (current < 0) return false;
  const auto current_offset = static_cast<std::uint64_t>(current);
  if (current_offset > payload_size || count > payload_size - current_offset ||
      count > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max())) {
    return false;
  }
  stream.seekg(static_cast<std::streamoff>(count), std::ios::cur);
  return stream && stream.tellg() ==
                       static_cast<std::streamoff>(current_offset + count);
}

struct ArtifactMetadata {
  JitArtifactKey key;
  std::uint64_t serialized_bytes{};
};

[[nodiscard]] std::optional<ArtifactMetadata> read_artifact_metadata(
    std::istream &stream, std::uint64_t record_offset,
    std::uint64_t payload_size) {
  const auto key = read_key(stream);
  const auto ir_size = read_u32(stream);
  const auto relocation_count = read_u32(stream);
  const auto exit_count = read_u32(stream);
  const auto dependency_count = read_u32(stream);
  const auto constant_dependency_count = read_u32(stream);
  const auto instruction_count = read_u32(stream);
  const auto translation_nanoseconds = read_u64(stream);
  if (!key || !ir_size || !relocation_count || !exit_count ||
      !dependency_count || !constant_dependency_count || !instruction_count ||
      !translation_nanoseconds ||
      *relocation_count > maximum_metadata_entries ||
      *exit_count > maximum_metadata_entries ||
      *dependency_count > maximum_metadata_entries ||
      *constant_dependency_count > maximum_metadata_entries) {
    return std::nullopt;
  }
  const auto record_bytes = serialized_artifact_bytes(
      *ir_size, *relocation_count, *exit_count, *dependency_count,
      *constant_dependency_count);
  if (!record_bytes ||
      *record_bytes > std::numeric_limits<std::uint64_t>::max() ||
      record_offset >
          std::numeric_limits<std::uint64_t>::max() - *record_bytes) {
    return std::nullopt;
  }
  if (!skip_bytes(stream, *ir_size, payload_size) ||
      !skip_bytes(stream,
                  static_cast<std::uint64_t>(*relocation_count) *
                      sizeof(std::uint64_t),
                  payload_size) ||
      !skip_bytes(stream,
                  static_cast<std::uint64_t>(*exit_count) *
                      sizeof(std::uint64_t),
                  payload_size)) {
    return std::nullopt;
  }
  for (std::uint32_t index = 0; index < *dependency_count; ++index) {
    const auto address = read_u32(stream);
    const auto size = read_u32(stream);
    if (!address || !size || *size == 0 ||
        !skip_bytes(stream, ContentIdentity{}.digest.size(), payload_size) ||
        !skip_bytes(stream, ContentIdentity{}.digest.size(), payload_size)) {
      return std::nullopt;
    }
  }
  for (std::uint32_t index = 0; index < *constant_dependency_count; ++index) {
    const auto address = read_u32(stream);
    const auto size = read_u32(stream);
    const auto value = read_u64(stream);
    if (!address || !size || *size == 0 || !value ||
        !skip_bytes(stream, ContentIdentity{}.digest.size(), payload_size) ||
        !skip_bytes(stream, ContentIdentity{}.digest.size(), payload_size)) {
      return std::nullopt;
    }
  }
  const auto end = stream.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) < record_offset ||
      static_cast<std::uint64_t>(end) - record_offset != *record_bytes) {
    return std::nullopt;
  }
  return ArtifactMetadata{*key, static_cast<std::uint64_t>(*record_bytes)};
}

struct SnapshotArtifactEntry {
  JitArtifactKey key;
  std::uint64_t offset{};
  std::uint64_t serialized_bytes{};
  ContentIdentity checksum;
};

[[nodiscard]] std::optional<std::vector<SnapshotArtifactEntry>>
read_snapshot_index(const std::filesystem::path &path,
                    std::uint64_t file_size) {
  if (file_size < artifact_header_bytes + artifact_index_header_bytes +
                      artifact_footer_bytes) {
    return std::nullopt;
  }
  auto source = ArtifactReadHandle::open(path);
  if (!source) return std::nullopt;
  auto stream = source->stream();
  if (!stream) return std::nullopt;
  std::array<char, artifact_magic.size()> magic{};
  stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  const auto header_count = read_u32(stream);
  if (!stream || magic != artifact_magic || !header_count ||
      *header_count > maximum_artifacts) {
    return std::nullopt;
  }

  const auto footer_offset = file_size - artifact_footer_bytes;
  if (footer_offset > static_cast<std::uint64_t>(
                          std::numeric_limits<std::streamoff>::max())) {
    return std::nullopt;
  }
  stream.seekg(static_cast<std::streamoff>(footer_offset));
  std::array<char, artifact_footer_magic.size()> footer_magic{};
  stream.read(footer_magic.data(),
              static_cast<std::streamsize>(footer_magic.size()));
  const auto index_offset = read_u64(stream);
  const auto index_bytes = read_u64(stream);
  const auto footer_count = read_u32(stream);
  ContentIdentity expected_index_checksum;
  if (!index_offset || !index_bytes || !footer_count ||
      !read_identity(stream, expected_index_checksum) ||
      footer_magic != artifact_footer_magic ||
      *footer_count != *header_count) {
    return std::nullopt;
  }
  const auto expected_index_bytes =
      static_cast<std::uint64_t>(artifact_index_header_bytes) +
      static_cast<std::uint64_t>(*header_count) *
          artifact_index_entry_bytes;
  if (*index_bytes != expected_index_bytes ||
      *index_offset < artifact_header_bytes ||
      *index_offset > footer_offset ||
      *index_bytes != footer_offset - *index_offset) {
    return std::nullopt;
  }
  const auto actual_index_checksum =
      sha256_file(source->descriptor(), *index_offset, *index_bytes);
  if (!actual_index_checksum ||
      *actual_index_checksum != expected_index_checksum ||
      *index_offset > static_cast<std::uint64_t>(
                          std::numeric_limits<std::streamoff>::max())) {
    return std::nullopt;
  }

  stream.clear();
  stream.seekg(static_cast<std::streamoff>(*index_offset));
  std::array<char, artifact_index_magic.size()> index_magic{};
  stream.read(index_magic.data(),
              static_cast<std::streamsize>(index_magic.size()));
  const auto index_count = read_u32(stream);
  if (!stream || index_magic != artifact_index_magic || !index_count ||
      *index_count != *header_count) {
    return std::nullopt;
  }
  const auto maximum_record_bytes = serialized_artifact_bytes(
      maximum_ir_bytes, maximum_metadata_entries,
      maximum_metadata_entries, maximum_metadata_entries,
      maximum_metadata_entries);
  if (!maximum_record_bytes) return std::nullopt;
  std::vector<SnapshotArtifactEntry> entries;
  entries.reserve(*index_count);
  std::uint64_t expected_record_offset = artifact_header_bytes;
  for (std::uint32_t index = 0; index < *index_count; ++index) {
    const auto key = read_key(stream);
    const auto record_offset = read_u64(stream);
    const auto record_bytes = read_u64(stream);
    ContentIdentity checksum;
    if (!key || !record_offset || !record_bytes || *record_bytes == 0U ||
        *record_bytes > *maximum_record_bytes ||
        !read_identity(stream, checksum) ||
        *record_offset != expected_record_offset ||
        *record_offset > *index_offset ||
        *record_bytes > *index_offset - *record_offset) {
      return std::nullopt;
    }
    expected_record_offset += *record_bytes;
    entries.push_back(SnapshotArtifactEntry{
        *key, *record_offset, *record_bytes, checksum});
  }
  if (expected_record_offset != *index_offset ||
      stream.tellg() != static_cast<std::streamoff>(footer_offset)) {
    return std::nullopt;
  }
  return entries;
}

[[nodiscard]] std::filesystem::path current_snapshot_path(
    const std::filesystem::path &path) {
  // F7 is intentionally not layout-compatible with prior snapshots. Keep an
  // unrecognized user cache and its journal intact while all current writers
  // converge on the same versioned sibling.
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error) return path;
  std::ifstream stream{path, std::ios::binary};
  std::array<char, artifact_magic.size()> magic{};
  if (stream) {
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (stream && magic == artifact_magic) return path;
  }
  return std::filesystem::path{path.string() + ".indexed-v1"};
}

[[nodiscard]] std::filesystem::path append_path_for(
    const std::filesystem::path &path) {
  return std::filesystem::path{path.string() + ".append"};
}

[[nodiscard]] bool has_storage_headroom(
    const std::filesystem::path &path, std::uintmax_t minimum_free_bytes,
    std::uintmax_t additional_bytes) {
  if (minimum_free_bytes == 0U) return true;
  std::error_code error;
  auto candidate = std::filesystem::absolute(path, error);
  if (error || candidate.empty()) candidate = path;
  for (;;) {
    const auto status = std::filesystem::status(candidate, error);
    if (!error && status.type() != std::filesystem::file_type::not_found) {
      break;
    }
    const auto parent = candidate.parent_path();
    if (parent.empty() || parent == candidate) return false;
    candidate = parent;
    error.clear();
  }
  const auto space = std::filesystem::space(candidate, error);
  if (error || space.available < minimum_free_bytes) return false;
  return additional_bytes <= space.available - minimum_free_bytes;
}

struct JournalArtifactEntry {
  JitArtifactKey key;
  std::uint64_t offset{};
  std::uint64_t serialized_bytes{};
};

struct ArtifactJournalScan {
  bool exists{};
  bool header_valid{};
  std::uint64_t file_size{};
  std::uint64_t valid_bytes{};
  std::vector<JournalArtifactEntry> entries;
};

[[nodiscard]] ArtifactJournalScan scan_artifact_journal(
    const std::filesystem::path &path) {
  ArtifactJournalScan result;
  std::error_code error;
  const auto file_size = std::filesystem::file_size(path, error);
  if (error) {
    result.exists = error != std::errc::no_such_file_or_directory;
    return result;
  }
  result.exists = true;
  if (file_size > std::numeric_limits<std::uint64_t>::max() ||
      file_size < artifact_append_magic.size()) {
    return result;
  }
  result.file_size = static_cast<std::uint64_t>(file_size);
  std::ifstream stream{path, std::ios::binary};
  if (!stream) return result;
  std::array<char, artifact_append_magic.size()> magic{};
  stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  if (!stream || magic != artifact_append_magic) return result;
  result.header_valid = true;
  result.valid_bytes = artifact_append_magic.size();

  while (result.valid_bytes < result.file_size) {
    const auto segment_offset_position = stream.tellg();
    if (segment_offset_position < 0) break;
    const auto segment_offset =
        static_cast<std::uint64_t>(segment_offset_position);
    const auto count = read_u32(stream);
    if (!count || *count > maximum_artifacts) break;
    std::vector<JournalArtifactEntry> segment_entries;
    segment_entries.reserve(*count);
    bool complete = true;
    for (std::uint32_t index = 0; index < *count; ++index) {
      const auto record_offset_position = stream.tellg();
      if (record_offset_position < 0) {
        complete = false;
        break;
      }
      const auto record_offset =
          static_cast<std::uint64_t>(record_offset_position);
      const auto metadata =
          read_artifact_metadata(stream, record_offset, result.file_size);
      if (!metadata) {
        complete = false;
        break;
      }
      segment_entries.push_back(
          JournalArtifactEntry{metadata->key, record_offset,
                               metadata->serialized_bytes});
    }
    if (!complete) break;
    const auto checksum_offset_position = stream.tellg();
    if (checksum_offset_position < 0) break;
    const auto checksum_offset =
        static_cast<std::uint64_t>(checksum_offset_position);
    if (result.file_size < artifact_checksum_bytes ||
        checksum_offset > result.file_size - artifact_checksum_bytes) {
      break;
    }
    ContentIdentity expected_checksum;
    stream.read(reinterpret_cast<char *>(expected_checksum.digest.data()),
                static_cast<std::streamsize>(expected_checksum.digest.size()));
    if (!stream) break;
    const auto checksum = sha256_file(
        path, segment_offset, checksum_offset - segment_offset);
    if (!checksum || *checksum != expected_checksum) break;
    result.entries.insert(result.entries.end(), segment_entries.begin(),
                          segment_entries.end());
    result.valid_bytes = checksum_offset + artifact_checksum_bytes;
  }
  return result;
}

[[nodiscard]] std::optional<std::shared_ptr<const BlockArtifact>>
read_artifact_at(const std::filesystem::path &path, std::uint64_t offset,
                 std::uint64_t expected_bytes,
                 const ContentIdentity *expected_checksum = nullptr) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max()) ||
      expected_bytes > std::numeric_limits<std::uint64_t>::max() - offset) {
    return std::nullopt;
  }
  auto source = ArtifactReadHandle::open(path);
  if (!source) return std::nullopt;
  if (expected_checksum != nullptr) {
    const auto actual_checksum =
        sha256_file(source->descriptor(), offset, expected_bytes);
    if (!actual_checksum || *actual_checksum != *expected_checksum) {
      return std::nullopt;
    }
  }
  auto stream = source->stream();
  if (!stream) return std::nullopt;
  stream.seekg(static_cast<std::streamoff>(offset));
  if (!stream) return std::nullopt;

  const auto key = read_key(stream);
  const auto ir_size = read_u32(stream);
  const auto relocation_count = read_u32(stream);
  const auto exit_count = read_u32(stream);
  const auto dependency_count = read_u32(stream);
  const auto constant_dependency_count = read_u32(stream);
  const auto instruction_count = read_u32(stream);
  const auto translation_nanoseconds = read_u64(stream);
  if (!key || !ir_size || !relocation_count || !exit_count ||
      !dependency_count || !constant_dependency_count || !instruction_count ||
      !translation_nanoseconds ||
      *relocation_count > maximum_metadata_entries ||
      *exit_count > maximum_metadata_entries ||
      *dependency_count > maximum_metadata_entries ||
      *constant_dependency_count > maximum_metadata_entries) {
    return std::nullopt;
  }

  const auto ir = read_bytes(stream, *ir_size);
  if (!ir) return std::nullopt;
  JitArtifactData data;
  data.normalized_ir = *ir;
  data.instruction_count = *instruction_count;
  data.translation_nanoseconds = *translation_nanoseconds;
  data.relocation_targets.reserve(*relocation_count);
  for (std::uint32_t index = 0; index < *relocation_count; ++index) {
    const auto target = read_u64(stream);
    if (!target) return std::nullopt;
    data.relocation_targets.push_back(*target);
  }
  data.exit_locations.reserve(*exit_count);
  for (std::uint32_t index = 0; index < *exit_count; ++index) {
    const auto location = read_u64(stream);
    if (!location) return std::nullopt;
    data.exit_locations.push_back(*location);
  }
  data.code_dependencies.reserve(*dependency_count);
  for (std::uint32_t index = 0; index < *dependency_count; ++index) {
    const auto address = read_u32(stream);
    const auto size = read_u32(stream);
    JitCodeDependency dependency;
    if (!address || !size || *size == 0 ||
        !read_identity(stream, dependency.content_identity) ||
        !read_identity(stream, dependency.layout_identity)) {
      return std::nullopt;
    }
    dependency.address = *address;
    dependency.size = *size;
    data.code_dependencies.push_back(std::move(dependency));
  }
  data.constant_dependencies.reserve(*constant_dependency_count);
  for (std::uint32_t index = 0; index < *constant_dependency_count; ++index) {
    const auto address = read_u32(stream);
    const auto size = read_u32(stream);
    const auto value = read_u64(stream);
    JitConstantDependency dependency;
    if (!address || !size || *size == 0 || !value ||
        !read_identity(stream, dependency.content_identity) ||
        !read_identity(stream, dependency.layout_identity)) {
      return std::nullopt;
    }
    dependency.address = *address;
    dependency.size = *size;
    dependency.value = *value;
    data.constant_dependencies.push_back(std::move(dependency));
  }
  const auto actual_bytes = serialized_artifact_bytes(data);
  const auto end = stream.tellg();
  if (!actual_bytes || *actual_bytes != expected_bytes || end < 0 ||
      static_cast<std::uint64_t>(end) != offset + expected_bytes) {
    return std::nullopt;
  }
  return std::make_shared<const BlockArtifact>(
      BlockArtifact{*key, std::move(data)});
}

bool write_artifact(std::ostream &stream, const BlockArtifact &artifact) {
  if (!serialized_artifact_bytes(artifact.data)) return false;
  write_key(stream, artifact.key);
  write_u32(stream,
            static_cast<std::uint32_t>(artifact.data.normalized_ir.size()));
  write_u32(stream, static_cast<std::uint32_t>(
                             artifact.data.relocation_targets.size()));
  write_u32(stream,
            static_cast<std::uint32_t>(artifact.data.exit_locations.size()));
  write_u32(stream, static_cast<std::uint32_t>(
                             artifact.data.code_dependencies.size()));
  write_u32(stream, static_cast<std::uint32_t>(
                             artifact.data.constant_dependencies.size()));
  write_u32(stream, artifact.data.instruction_count);
  write_u64(stream, artifact.data.translation_nanoseconds);
  stream.write(
      reinterpret_cast<const char *>(artifact.data.normalized_ir.data()),
      static_cast<std::streamsize>(artifact.data.normalized_ir.size()));
  for (const auto target : artifact.data.relocation_targets)
    write_u64(stream, target);
  for (const auto location : artifact.data.exit_locations)
    write_u64(stream, location);
  for (const auto &dependency : artifact.data.code_dependencies) {
    write_u32(stream, dependency.address);
    write_u32(stream, dependency.size);
    write_identity(stream, dependency.content_identity);
    write_identity(stream, dependency.layout_identity);
  }
  for (const auto &dependency : artifact.data.constant_dependencies) {
    write_u32(stream, dependency.address);
    write_u32(stream, dependency.size);
    write_u64(stream, dependency.value);
    write_identity(stream, dependency.content_identity);
    write_identity(stream, dependency.layout_identity);
  }
  return static_cast<bool>(stream);
}

bool copy_disk_record(std::istream &source, std::ostream &destination,
                      std::uint64_t offset, std::uint64_t byte_count) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max()) ||
      byte_count > static_cast<std::uint64_t>(
                       std::numeric_limits<std::streamsize>::max())) {
    return false;
  }
  source.clear();
  source.seekg(static_cast<std::streamoff>(offset));
  if (!source) return false;
  std::array<char, 64U * 1024U> buffer{};
  auto remaining = byte_count;
  while (remaining != 0U) {
    const auto requested = std::min<std::uint64_t>(remaining, buffer.size());
    source.read(buffer.data(), static_cast<std::streamsize>(requested));
    if (source.gcount() != static_cast<std::streamsize>(requested)) {
      return false;
    }
    destination.write(buffer.data(), static_cast<std::streamsize>(requested));
    if (!destination) return false;
    remaining -= requested;
  }
  return true;
}

} // namespace

std::size_t JitArtifactKeyHash::operator()(
    const JitArtifactKey &key) const noexcept {
  auto hash = std::size_t{0};
  hash_identity(hash, key.content_identity);
  hash_identity(hash, key.layout_identity);
  hash_scalar(hash, key.guest_pc);
  hash_scalar(hash, key.thumb);
  hash_scalar(hash, key.location_descriptor);
  hash_scalar(hash, key.architecture);
  hash_scalar(hash, key.cpu_model);
  hash_scalar(hash, key.timing_model_version);
  hash_scalar(hash, key.guest_ticks_per_second);
  hash_scalar(hash, key.image_slide);
  hash_scalar(hash, key.hle_abi_version);
  hash_scalar(hash, key.backend_abi_version);
  hash_scalar(hash, key.dynarmic_build_fingerprint);
  hash_scalar(hash, key.codegen_options);
  hash_scalar(hash, key.host_isa);
  hash_scalar(hash, key.host_feature_mask);
  hash_scalar(hash, key.artifact_format_version);
  return hash;
}

JitArtifactStore::JitArtifactStore(
    std::filesystem::path persistence_path, JitArtifactLimits limits)
    : limits_{std::move(limits)}, persistence_path_{std::move(persistence_path)} {
  bool persistence_ready = false;
  if (!persistence_path_.empty() && limits_.persistence_enabled) {
    persistence_path_ = current_snapshot_path(persistence_path_);
    persistence_ready = load(persistence_path_);
    if (!persistence_ready) {
      persistence_ready = save(persistence_path_);
    }
  }
  writeback_disabled_ = !persistence_ready || limits_.writeback_bytes == 0U;
  if (!writeback_disabled_) {
    writeback_thread_ = std::thread{[this] { writeback_loop(); }};
  }
}

JitArtifactStore::~JitArtifactStore() {
  {
    const std::lock_guard lock{mutex_};
    writeback_stopping_ = true;
  }
  writeback_condition_.notify_all();
  if (writeback_thread_.joinable()) writeback_thread_.join();
  static_cast<void>(save());
}

std::shared_ptr<const BlockArtifact> JitArtifactStore::find(
    const JitArtifactKey &key) const {
  const auto record_matches = [](const DiskArtifactRecord &left,
                                 const DiskArtifactRecord &right) {
    return left.generation == right.generation &&
           left.offset == right.offset &&
           left.serialized_bytes == right.serialized_bytes &&
           left.append_log == right.append_log &&
           left.checksum_valid == right.checksum_valid &&
           (!left.checksum_valid || left.checksum == right.checksum);
  };
  constexpr unsigned maximum_record_attempts = 3U;
  std::shared_ptr<DiskReadFlight> flight;
  {
    std::unique_lock lock{mutex_};
    ++stats_.lookups;
    if (auto artifact = artifacts_.find(key);
        artifact != artifacts_.end()) {
      if (artifact->second.loaded_from_disk) {
        ++stats_.disk_hits;
        artifact->second.loaded_from_disk = false;
      } else {
        ++stats_.memory_hits;
      }
      touch_locked(artifact);
      return artifact->second.artifact;
    }
    if (const auto pending = pending_writebacks_.find(key);
        pending != pending_writebacks_.end()) {
      ++stats_.memory_hits;
      return pending->second.artifact;
    }
    const auto disk_artifact = disk_artifacts_.find(key);
    if (disk_artifact == disk_artifacts_.end() ||
        disk_artifact->second.serialized_bytes >
            std::numeric_limits<std::size_t>::max()) {
      ++stats_.misses;
      return nullptr;
    }
    if (const auto active = disk_read_flights_.find(key);
        active != disk_read_flights_.end()) {
      flight = active->second;
      ++stats_.disk_read_waits;
      flight->condition.wait(lock, [&flight] { return flight->complete; });
      const auto result = flight->artifact;
      if (!result) {
        ++stats_.misses;
        return nullptr;
      }
      if (flight->disk_hit) {
        ++stats_.disk_hits;
      } else {
        ++stats_.memory_hits;
      }
      if (auto resident = artifacts_.find(key);
          resident != artifacts_.end()) {
        touch_locked(resident);
      }
      return result;
    }
    flight = std::make_shared<DiskReadFlight>();
    disk_read_flights_.emplace(key, flight);
  }

  const auto finish_locked =
      [this, &key, &flight](std::unique_lock<std::mutex> &lock,
                            std::shared_ptr<const BlockArtifact> artifact,
                            bool disk_hit) {
        flight->artifact = artifact;
        flight->disk_hit = disk_hit;
        flight->complete = true;
        if (const auto active = disk_read_flights_.find(key);
            active != disk_read_flights_.end() &&
            active->second == flight) {
          disk_read_flights_.erase(active);
        }
        lock.unlock();
        flight->condition.notify_all();
        return artifact;
      };

  try {
    for (unsigned attempt = 0; attempt < maximum_record_attempts; ++attempt) {
      DiskArtifactRecord record;
      std::filesystem::path source_path;
      {
        std::unique_lock lock{mutex_};
        if (auto artifact = artifacts_.find(key);
            artifact != artifacts_.end()) {
          const auto disk_hit = artifact->second.loaded_from_disk;
          if (disk_hit) {
            ++stats_.disk_hits;
            artifact->second.loaded_from_disk = false;
          } else {
            ++stats_.memory_hits;
          }
          touch_locked(artifact);
          return finish_locked(lock, artifact->second.artifact, disk_hit);
        }
        if (const auto pending = pending_writebacks_.find(key);
            pending != pending_writebacks_.end()) {
          ++stats_.memory_hits;
          return finish_locked(lock, pending->second.artifact, false);
        }
        const auto disk_artifact = disk_artifacts_.find(key);
        if (disk_artifact == disk_artifacts_.end() ||
            disk_artifact->second.serialized_bytes >
                std::numeric_limits<std::size_t>::max()) {
          ++stats_.misses;
          return finish_locked(lock, nullptr, false);
        }
        record = disk_artifact->second;
        source_path = record.append_log ? disk_append_path_
                                        : disk_source_path_;
      }

      // The per-key flight owns retries while disk latency and deserialization
      // remain outside the global store mutex.
      const auto loaded = read_artifact_at(
          source_path, record.offset, record.serialized_bytes,
          record.checksum_valid ? &record.checksum : nullptr);

      std::unique_lock lock{mutex_};
      if (auto artifact = artifacts_.find(key);
          artifact != artifacts_.end()) {
        const auto disk_hit = artifact->second.loaded_from_disk;
        if (disk_hit) {
          ++stats_.disk_hits;
          artifact->second.loaded_from_disk = false;
        } else {
          ++stats_.memory_hits;
        }
        touch_locked(artifact);
        return finish_locked(lock, artifact->second.artifact, disk_hit);
      }
      if (const auto pending = pending_writebacks_.find(key);
          pending != pending_writebacks_.end()) {
        ++stats_.memory_hits;
        return finish_locked(lock, pending->second.artifact, false);
      }
      const auto current = disk_artifacts_.find(key);
      const auto current_path =
          current != disk_artifacts_.end()
              ? (current->second.append_log ? disk_append_path_
                                            : disk_source_path_)
              : std::filesystem::path{};
      if (current == disk_artifacts_.end() ||
          !record_matches(current->second, record) ||
          current_path != source_path) {
        if (attempt + 1U < maximum_record_attempts) {
          ++stats_.disk_read_retries;
          continue;
        }
        ++stats_.misses;
        return finish_locked(lock, nullptr, false);
      }
      if (!loaded || (*loaded)->key != key) {
        ++stats_.misses;
        return finish_locked(lock, nullptr, false);
      }
      insert_locked(*loaded, static_cast<std::size_t>(record.serialized_bytes),
                    true);
      auto artifact = artifacts_.find(key);
      if (artifact == artifacts_.end()) {
        ++stats_.misses;
        return finish_locked(lock, nullptr, false);
      }
      ++stats_.disk_hits;
      artifact->second.loaded_from_disk = false;
      touch_locked(artifact);
      return finish_locked(lock, artifact->second.artifact, true);
    }
  } catch (...) {
    std::unique_lock lock{mutex_};
    ++stats_.misses;
    return finish_locked(lock, nullptr, false);
  }
  std::unique_lock lock{mutex_};
  ++stats_.misses;
  return finish_locked(lock, nullptr, false);
}

void JitArtifactStore::touch_locked(ArtifactMap::iterator iterator) const {
  lru_.splice(lru_.end(), lru_, iterator->second.lru_position);
  iterator->second.lru_position = std::prev(lru_.end());
}

std::uint64_t JitArtifactStore::next_disk_generation_locked() const noexcept {
  return ++disk_index_generation_;
}

void JitArtifactStore::evict_until_fit_locked(
    std::size_t required_bytes) const {
  if (limits_.resident_bytes == 0U) return;
  while (resident_bytes_ > limits_.resident_bytes -
                              std::min(limits_.resident_bytes,
                                       required_bytes) &&
         !lru_.empty()) {
    const auto key = lru_.front();
    lru_.pop_front();
    const auto iterator = artifacts_.find(key);
    if (iterator == artifacts_.end()) continue;
    resident_bytes_ -= iterator->second.serialized_bytes;
    ++stats_.evictions;
    artifacts_.erase(iterator);
  }
}

void JitArtifactStore::insert_locked(
    std::shared_ptr<const BlockArtifact> artifact,
    std::size_t serialized_bytes, bool loaded_from_disk) const {
  const auto key = artifact->key;
  if (const auto existing = artifacts_.find(key); existing != artifacts_.end()) {
    touch_locked(existing);
    return;
  }
  if (limits_.resident_bytes != 0U &&
      serialized_bytes > limits_.resident_bytes) {
    return;
  }
  evict_until_fit_locked(serialized_bytes);
  if (limits_.resident_bytes != 0U &&
      resident_bytes_ > limits_.resident_bytes - serialized_bytes) {
    return;
  }
  if (serialized_bytes >
      std::numeric_limits<std::size_t>::max() - resident_bytes_) {
    return;
  }
  lru_.push_back(key);
  const auto lru_position = std::prev(lru_.end());
  const auto [iterator, inserted] = artifacts_.try_emplace(
      key, ArtifactRecord{std::move(artifact), serialized_bytes,
                          lru_position, loaded_from_disk});
  if (!inserted) {
    lru_.erase(lru_position);
    touch_locked(iterator);
    return;
  }
  resident_bytes_ += serialized_bytes;
  if (loaded_from_disk) ++stats_.disk_loaded_entries;
}

bool JitArtifactStore::enqueue_writeback_locked(
    const std::shared_ptr<const BlockArtifact> &artifact,
    std::size_t serialized_bytes) const {
  if (writeback_disabled_ || persistence_path_.empty() ||
      !limits_.persistence_enabled || limits_.writeback_bytes == 0U ||
      disk_artifacts_.contains(artifact->key) ||
      pending_writebacks_.contains(artifact->key)) {
    return false;
  }
  if (serialized_bytes > limits_.writeback_bytes ||
      pending_writeback_bytes_ >
          limits_.writeback_bytes - serialized_bytes) {
    ++stats_.writeback_dropped;
    return false;
  }
  writeback_order_.push_back(artifact->key);
  const auto queue_position = std::prev(writeback_order_.end());
  const auto [iterator, inserted] = pending_writebacks_.try_emplace(
      artifact->key,
      PendingWriteback{artifact, serialized_bytes, queue_position});
  if (!inserted) {
    writeback_order_.erase(queue_position);
    return false;
  }
  if (serialized_bytes >
      std::numeric_limits<std::size_t>::max() - pending_writeback_bytes_) {
    writeback_order_.erase(iterator->second.queue_position);
    pending_writebacks_.erase(iterator);
    ++stats_.writeback_dropped;
    return false;
  }
  pending_writeback_bytes_ += serialized_bytes;
  ++stats_.writeback_enqueued;
  return true;
}

void JitArtifactStore::retire_writeback_locked(
    const JitArtifactKey &key) const {
  const auto pending = pending_writebacks_.find(key);
  if (pending == pending_writebacks_.end()) return;
  pending_writeback_bytes_ -= pending->second.serialized_bytes;
  writeback_order_.erase(pending->second.queue_position);
  pending_writebacks_.erase(pending);
}

std::shared_ptr<const BlockArtifact> JitArtifactStore::publish(
    JitArtifactKey key, JitArtifactData data) {
  const auto artifact_bytes = serialized_artifact_bytes(data);
  if (!artifact_bytes) return nullptr;
  std::shared_ptr<const BlockArtifact> result;
  bool notify_writeback = false;
  {
    const std::lock_guard lock{mutex_};
    ++stats_.publish_calls;
    if (const auto existing = artifacts_.find(key);
        existing != artifacts_.end()) {
      ++stats_.deduplicated_publishes;
      touch_locked(existing);
      return existing->second.artifact;
    }
    if (const auto pending = pending_writebacks_.find(key);
        pending != pending_writebacks_.end()) {
      ++stats_.deduplicated_publishes;
      return pending->second.artifact;
    }
    auto artifact = std::make_shared<const BlockArtifact>(
        BlockArtifact{std::move(key), std::move(data)});
    const auto lookup_key = artifact->key;
    // The queue owns the immutable artifact before insert_locked can evict a
    // resident entry to make room for it.
    const auto requires_writeback =
        !writeback_disabled_ && !persistence_path_.empty() &&
        limits_.persistence_enabled && limits_.writeback_bytes != 0U &&
        !disk_artifacts_.contains(lookup_key);
    notify_writeback = enqueue_writeback_locked(artifact, *artifact_bytes);
    // Under queue pressure, degrade by declining the optional cache
    // publication. In particular, do not evict a retained artifact on behalf
    // of a new artifact that the bounded writer could not accept.
    if (!requires_writeback || notify_writeback) {
      insert_locked(artifact, *artifact_bytes);
    }
    const auto inserted = artifacts_.find(lookup_key);
    result = inserted != artifacts_.end()
                 ? inserted->second.artifact
                 : (notify_writeback ? artifact : nullptr);
  }
  if (notify_writeback) writeback_condition_.notify_one();
  return result;
}

std::size_t JitArtifactStore::size() const {
  const std::lock_guard lock{mutex_};
  std::size_t result = disk_artifacts_.size();
  for (const auto &artifact : artifacts_) {
    if (disk_artifacts_.find(artifact.first) == disk_artifacts_.end()) {
      ++result;
    }
  }
  for (const auto &pending : pending_writebacks_) {
    if (disk_artifacts_.find(pending.first) == disk_artifacts_.end() &&
        artifacts_.find(pending.first) == artifacts_.end()) {
      ++result;
    }
  }
  return result;
}

JitArtifactStoreStats JitArtifactStore::stats() const {
  JitArtifactStoreStats result;
  std::filesystem::path disk_path;
  std::filesystem::path append_path;
  {
    const std::lock_guard lock{mutex_};
    result = stats_;
    result.resident_bytes = resident_bytes_;
    result.writeback_pending_bytes = pending_writeback_bytes_;
    if (!limits_.persistence_enabled) return result;
    disk_path = !persistence_path_.empty() ? persistence_path_
                                           : disk_source_path_;
    append_path = !disk_append_path_.empty()
                      ? disk_append_path_
                      : append_path_for(disk_path);
  }
  if (!disk_path.empty()) {
    std::error_code error;
    result.disk_bytes = std::filesystem::file_size(disk_path, error);
    if (error) result.disk_bytes = 0;
    error.clear();
    const auto append_bytes = std::filesystem::file_size(append_path, error);
    if (!error && append_bytes <=
                      std::numeric_limits<std::uintmax_t>::max() -
                          result.disk_bytes) {
      result.disk_bytes += append_bytes;
    } else if (!error) {
      result.disk_bytes = std::numeric_limits<std::uintmax_t>::max();
    }
  }
  return result;
}

void JitArtifactStore::cancel_writeback() noexcept {
  if (writeback_cancel_requested_.exchange(true,
                                           std::memory_order_acq_rel)) {
    return;
  }
  {
    const std::lock_guard lock{mutex_};
    writeback_disabled_ = true;
    ++stats_.writeback_cancellations;
    stats_.writeback_dropped += pending_writebacks_.size();
    pending_writebacks_.clear();
    writeback_order_.clear();
    pending_writeback_bytes_ = 0;
  }
  writeback_condition_.notify_all();
}

void JitArtifactStore::writeback_loop() {
  constexpr std::size_t maximum_batch_bytes = 4U * 1024U * 1024U;
  for (;;) {
    std::vector<std::shared_ptr<const BlockArtifact>> batch;
    {
      std::unique_lock lock{mutex_};
      writeback_condition_.wait(lock, [this] {
        return writeback_stopping_ ||
               (!writeback_disabled_ && !pending_writebacks_.empty());
      });
      if (writeback_stopping_ &&
          (pending_writebacks_.empty() || writeback_disabled_)) {
        return;
      }

      for (auto key = writeback_order_.begin();
           key != writeback_order_.end();) {
        const auto pending = pending_writebacks_.find(*key);
        if (pending == pending_writebacks_.end()) {
          key = writeback_order_.erase(key);
          continue;
        }
        if (disk_artifacts_.contains(*key)) {
          const auto persisted_key = *key;
          ++key;
          retire_writeback_locked(persisted_key);
          continue;
        }
        std::size_t batch_bytes = 0;
        for (auto candidate = key; candidate != writeback_order_.end();
             ++candidate) {
          const auto entry = pending_writebacks_.find(*candidate);
          if (entry == pending_writebacks_.end() ||
              disk_artifacts_.contains(*candidate)) {
            continue;
          }
          const auto bytes = entry->second.serialized_bytes;
          if (!batch.empty() &&
              (bytes > maximum_batch_bytes ||
               batch_bytes > maximum_batch_bytes - bytes)) {
            break;
          }
          batch.push_back(entry->second.artifact);
          batch_bytes += bytes;
        }
        break;
      }
      if (batch.empty()) {
        if (writeback_stopping_ && pending_writebacks_.empty()) return;
        continue;
      }
    }

    const auto saved = append_writeback_batch(batch);
    const std::lock_guard lock{mutex_};
    if (!saved) {
      if (writeback_cancel_requested_.load(std::memory_order_acquire)) {
        continue;
      }
      ++stats_.writeback_failures;
      stats_.writeback_dropped += pending_writebacks_.size();
      pending_writebacks_.clear();
      writeback_order_.clear();
      pending_writeback_bytes_ = 0;
      writeback_disabled_ = true;
      continue;
    }
    for (const auto &artifact : batch) {
      if (disk_artifacts_.contains(artifact->key)) {
        retire_writeback_locked(artifact->key);
        ++stats_.writeback_saved;
      }
    }
  }
}

bool JitArtifactStore::append_writeback_batch(
    const std::vector<std::shared_ptr<const BlockArtifact>> &batch) const
    noexcept {
  try {
    if (batch.empty()) return true;
    if (!limits_.persistence_enabled || persistence_path_.empty()) {
      return false;
    }
    if (writeback_cancel_requested_.load(std::memory_order_acquire)) {
      return false;
    }
    const std::lock_guard persistence_lock{persistence_mutex_};
    if (writeback_cancel_requested_.load(std::memory_order_acquire)) {
      return false;
    }
    auto file_lock = ArtifactFileLock::acquire(
        persistence_path_, ArtifactFileLock::Mode::Exclusive);
    if (!file_lock) return false;
    const auto writer_generation = file_lock->generation();
    if (!writer_generation) return false;
    std::uint64_t known_generation = 0;
    {
      const std::lock_guard lock{mutex_};
      known_generation = external_writer_generation_;
    }
    if (known_generation != *writer_generation) {
      if (!load_coordinated(persistence_path_)) return false;
      const std::lock_guard lock{mutex_};
      external_writer_generation_ = *writer_generation;
    }
    if (writeback_cancel_requested_.load(std::memory_order_acquire)) {
      return false;
    }
    const auto next_writer_generation = file_lock->begin_write();
    if (!next_writer_generation) return false;
    {
      const std::lock_guard lock{mutex_};
      external_writer_generation_ = *next_writer_generation;
    }

    std::vector<std::shared_ptr<const BlockArtifact>> artifacts;
    {
      const std::lock_guard lock{mutex_};
      artifacts.reserve(batch.size());
      for (const auto &artifact : batch) {
        if (!disk_artifacts_.contains(artifact->key)) {
          artifacts.push_back(artifact);
        }
      }
    }
    if (artifacts.empty()) return true;
    if (artifacts.size() > maximum_artifacts) return false;

    std::error_code base_error;
    const auto base_size =
        std::filesystem::file_size(persistence_path_, base_error);
    if (base_error || base_size > maximum_persistence_bytes) return false;
    const auto configured_limit =
        limits_.persistence_bytes == 0U
            ? maximum_persistence_bytes
            : std::min<std::uintmax_t>(limits_.persistence_bytes,
                                       maximum_persistence_bytes);
    if (base_size > configured_limit) return false;

    const auto append_path = append_path_for(persistence_path_);
    std::uint64_t journal_size = 0;
    {
      const std::lock_guard lock{mutex_};
      if (disk_artifacts_.size() > maximum_artifacts - artifacts.size()) {
        return false;
      }
      journal_size = disk_append_valid_bytes_;
    }
    if (journal_size == 0U) {
      const auto parent = append_path.parent_path();
      if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) return false;
      }
      std::ofstream initialize{append_path,
                               std::ios::binary | std::ios::trunc};
      if (!initialize) return false;
      initialize.write(
          artifact_append_magic.data(),
          static_cast<std::streamsize>(artifact_append_magic.size()));
      initialize.flush();
      if (!initialize) return false;
      initialize.close();
      journal_size = artifact_append_magic.size();
      const std::lock_guard lock{mutex_};
      disk_append_path_ = append_path;
      disk_append_valid_bytes_ = journal_size;
    } else {
      std::error_code journal_error;
      const auto actual_size =
          std::filesystem::file_size(append_path, journal_error);
      if (journal_error || actual_size < journal_size) return false;
      if (actual_size > journal_size) {
        std::filesystem::resize_file(append_path, journal_size,
                                     journal_error);
        if (journal_error) return false;
      }
    }

    std::vector<std::uint64_t> record_bytes;
    record_bytes.reserve(artifacts.size());
    std::uint64_t segment_bytes = sizeof(std::uint32_t);
    for (const auto &artifact : artifacts) {
      const auto serialized = serialized_artifact_bytes(artifact->data);
      if (!serialized ||
          *serialized > std::numeric_limits<std::uint64_t>::max()) {
        return false;
      }
      const auto bytes = static_cast<std::uint64_t>(*serialized);
      if (bytes > std::numeric_limits<std::uint64_t>::max() -
                      segment_bytes) {
        return false;
      }
      segment_bytes += bytes;
      record_bytes.push_back(bytes);
    }
    if (segment_bytes > std::numeric_limits<std::uint64_t>::max() -
                            artifact_checksum_bytes) {
      return false;
    }
    const auto append_bytes =
        segment_bytes + static_cast<std::uint64_t>(artifact_checksum_bytes);
    if (journal_size > std::numeric_limits<std::uint64_t>::max() -
                           append_bytes) {
      return false;
    }
    const auto journal_end = journal_size + append_bytes;
    if (journal_end > maximum_persistence_bytes ||
        journal_end > configured_limit ||
        base_size > configured_limit - journal_size ||
        append_bytes > configured_limit - base_size - journal_size ||
        !has_storage_headroom(
            persistence_path_, limits_.minimum_free_bytes,
            static_cast<std::uintmax_t>(append_bytes))) {
      return false;
    }

    std::ofstream stream{append_path, std::ios::binary | std::ios::app};
    if (!stream) return false;
    const auto segment_position = stream.tellp();
    if (segment_position < 0 ||
        static_cast<std::uint64_t>(segment_position) != journal_size) {
      return false;
    }
    const auto segment_offset =
        static_cast<std::uint64_t>(segment_position);
    write_u32(stream, static_cast<std::uint32_t>(artifacts.size()));
    std::uint64_t record_offset = segment_offset + sizeof(std::uint32_t);
    std::vector<DiskArtifactRecord> records;
    records.reserve(artifacts.size());
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
      if (writeback_cancel_requested_.load(std::memory_order_acquire)) {
        return false;
      }
      if (!write_artifact(stream, *artifacts[index])) return false;
      records.push_back(
          DiskArtifactRecord{record_offset, record_bytes[index], true, {},
                             false, 0U});
      if (record_offset > std::numeric_limits<std::uint64_t>::max() -
                              record_bytes[index]) {
        return false;
      }
      record_offset += record_bytes[index];
    }
    stream.flush();
    if (!stream ||
        stream.tellp() != static_cast<std::streamoff>(record_offset)) {
      return false;
    }
    if (writeback_cancel_requested_.load(std::memory_order_acquire)) {
      return false;
    }
    const auto checksum = sha256_file(
        append_path, segment_offset, record_offset - segment_offset);
    if (!checksum) return false;
    if (writeback_cancel_requested_.load(std::memory_order_acquire)) {
      return false;
    }
    stream.write(reinterpret_cast<const char *>(checksum->digest.data()),
                 static_cast<std::streamsize>(checksum->digest.size()));
    stream.flush();
    if (!stream ||
        stream.tellp() != static_cast<std::streamoff>(journal_end)) {
      return false;
    }
    stream.close();
    if (!stream) return false;

    const std::lock_guard lock{mutex_};
    disk_source_path_ = persistence_path_;
    disk_append_path_ = append_path;
    disk_append_valid_bytes_ = journal_end;
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
      records[index].generation = next_disk_generation_locked();
      disk_artifacts_[artifacts[index]->key] = records[index];
      disk_order_.push_back(artifacts[index]->key);
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool JitArtifactStore::compaction_needed() const noexcept {
  try {
    if (!limits_.persistence_enabled || limits_.compaction_bytes == 0U) {
      return false;
    }
    std::filesystem::path disk_path;
    {
      const std::lock_guard lock{mutex_};
      disk_path = !persistence_path_.empty() ? persistence_path_
                                             : disk_source_path_;
    }
    if (disk_path.empty()) return false;
    std::error_code error;
    const auto append_bytes = std::filesystem::file_size(
        append_path_for(disk_path), error);
    return !error && append_bytes >= limits_.compaction_bytes;
  } catch (...) {
    return false;
  }
}

bool JitArtifactStore::compact() const noexcept {
  try {
    if (!limits_.persistence_enabled || limits_.compaction_bytes == 0U) {
      return true;
    }
    std::filesystem::path disk_path;
    {
      const std::lock_guard lock{mutex_};
      disk_path = !persistence_path_.empty() ? persistence_path_
                                             : disk_source_path_;
    }
    if (disk_path.empty()) return true;
    {
      const std::lock_guard persistence_lock{persistence_mutex_};
      auto file_lock = ArtifactFileLock::acquire(
          disk_path, ArtifactFileLock::Mode::Exclusive);
      if (!file_lock) return false;
      const auto writer_generation = file_lock->generation();
      if (!writer_generation) return false;
      std::uint64_t known_generation = 0;
      {
        const std::lock_guard lock{mutex_};
        known_generation = external_writer_generation_;
      }
      if (known_generation != *writer_generation) {
        if (!load_coordinated(disk_path)) return false;
        const std::lock_guard lock{mutex_};
        external_writer_generation_ = *writer_generation;
      }
      std::error_code error;
      const auto append_bytes = std::filesystem::file_size(
          append_path_for(disk_path), error);
      if (error || append_bytes < limits_.compaction_bytes) return true;
      const auto next_writer_generation = file_lock->begin_write();
      if (!next_writer_generation) return false;
      {
        const std::lock_guard lock{mutex_};
        external_writer_generation_ = *next_writer_generation;
      }
      if (!save_full(disk_path)) return false;
    }
    const std::lock_guard lock{mutex_};
    ++stats_.compactions;
    return true;
  } catch (...) {
    return false;
  }
}

bool JitArtifactStore::load(const std::filesystem::path &path) noexcept {
  if (!limits_.persistence_enabled || path.empty()) return false;
  const std::lock_guard persistence_lock{persistence_mutex_};
  auto file_lock =
      ArtifactFileLock::acquire(path, ArtifactFileLock::Mode::Shared);
  if (!file_lock) return false;
  const auto writer_generation = file_lock->generation();
  if (!writer_generation || !load_coordinated(path)) return false;
  const std::lock_guard lock{mutex_};
  external_writer_generation_ = *writer_generation;
  return true;
}

bool JitArtifactStore::load_coordinated(
    const std::filesystem::path &path) const noexcept {
  try {
    if (!limits_.persistence_enabled) return false;
    struct DiskEntry {
      JitArtifactKey key;
      DiskArtifactRecord record;
    };

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) return false;
    const auto configured_limit = limits_.persistence_bytes == 0U
                                      ? maximum_persistence_bytes
                                      : std::min<std::uintmax_t>(
                                            limits_.persistence_bytes,
                                            maximum_persistence_bytes);
    if (file_size > configured_limit) return false;
    if (file_size > std::numeric_limits<std::uint64_t>::max()) return false;
    const auto indexed = read_snapshot_index(
        path, static_cast<std::uint64_t>(file_size));
    if (!indexed) return false;

    DiskArtifactMap loaded_index;
    loaded_index.reserve(indexed->size());
    std::vector<DiskEntry> scanned;
    scanned.reserve(indexed->size());
    for (const auto &entry : *indexed) {
      const DiskArtifactRecord record{entry.offset,
                                      entry.serialized_bytes,
                                      false,
                                      entry.checksum,
                                      true,
                                      0U};
      if (!loaded_index.emplace(entry.key, record).second) return false;
      scanned.push_back(DiskEntry{entry.key, record});
    }

    const auto append_path = append_path_for(path);
    const auto journal = scan_artifact_journal(append_path);
    if (journal.exists &&
        journal.file_size > configured_limit - file_size) {
      return false;
    }
    if (journal.header_valid) {
      for (const auto &entry : journal.entries) {
        const DiskArtifactRecord record{entry.offset,
                                        entry.serialized_bytes, true, {},
                                        false, 0U};
        loaded_index[entry.key] = record;
        scanned.push_back(DiskEntry{entry.key, record});
      }
    }
    std::vector<DiskEntry> unique;
    unique.reserve(scanned.size());
    for (const auto &entry : scanned) {
      const auto latest = loaded_index.find(entry.key);
      if (latest != loaded_index.end() &&
          latest->second.offset == entry.record.offset &&
          latest->second.append_log == entry.record.append_log) {
        unique.push_back(entry);
      }
    }

    std::vector<DiskEntry> preload;
    std::size_t preload_bytes = 0;
    for (auto iterator = unique.rbegin(); iterator != unique.rend();
         ++iterator) {
      if (iterator->record.serialized_bytes >
          std::numeric_limits<std::size_t>::max()) {
        continue;
      }
      const auto bytes = static_cast<std::size_t>(
          iterator->record.serialized_bytes);
      if (limits_.resident_bytes != 0U &&
          (bytes > limits_.resident_bytes ||
           preload_bytes > limits_.resident_bytes - bytes)) {
        continue;
      }
      if (bytes > std::numeric_limits<std::size_t>::max() - preload_bytes) {
        return false;
      }
      preload_bytes += bytes;
      preload.push_back(*iterator);
    }
    std::reverse(preload.begin(), preload.end());
    std::vector<std::shared_ptr<const BlockArtifact>> loaded;
    loaded.reserve(preload.size());
    for (const auto &entry : preload) {
      const auto &source_path =
          entry.record.append_log ? append_path : path;
      const auto artifact = read_artifact_at(
          source_path, entry.record.offset, entry.record.serialized_bytes,
          entry.record.checksum_valid ? &entry.record.checksum : nullptr);
      if (!artifact || (*artifact)->key != entry.key) return false;
      loaded.push_back(*artifact);
    }
    std::vector<JitArtifactKey> loaded_order;
    loaded_order.reserve(unique.size());
    for (const auto &entry : unique) loaded_order.push_back(entry.key);
    std::filesystem::path loaded_source_path{path};
    std::filesystem::path loaded_append_path{append_path};

    const std::lock_guard lock{mutex_};
    for (auto &entry : loaded_index) {
      entry.second.generation = next_disk_generation_locked();
    }
    disk_artifacts_ = std::move(loaded_index);
    disk_order_ = std::move(loaded_order);
    disk_source_path_ = std::move(loaded_source_path);
    disk_append_path_ = std::move(loaded_append_path);
    disk_append_valid_bytes_ =
        journal.header_valid ? journal.valid_bytes : 0U;
    for (auto &artifact : loaded) {
      const auto artifact_bytes = serialized_artifact_bytes(artifact->data);
      if (artifact_bytes)
        insert_locked(std::move(artifact), *artifact_bytes, true);
    }
    return true;
  } catch (...) {
    return false;
  }
}

JitArtifactStore::AppendResult JitArtifactStore::append_new_artifacts(
    const std::filesystem::path &path) const noexcept {
  try {
    if (!limits_.persistence_enabled) return AppendResult::Failed;
    if (path.empty()) return AppendResult::Failed;

    {
      const std::lock_guard lock{mutex_};
      if (disk_source_path_.empty() || disk_source_path_ != path) {
        return AppendResult::NotApplicable;
      }
    }

    std::error_code base_error;
    const auto base_size = std::filesystem::file_size(path, base_error);
    if (base_error) return AppendResult::NotApplicable;
    if (base_size > maximum_persistence_bytes) return AppendResult::Failed;

    const auto configured_limit =
        limits_.persistence_bytes == 0U
            ? maximum_persistence_bytes
            : std::min<std::uintmax_t>(limits_.persistence_bytes,
                                       maximum_persistence_bytes);
    if (base_size > configured_limit) return AppendResult::Failed;

    const auto append_path = append_path_for(path);
    auto journal = scan_artifact_journal(append_path);
    std::vector<std::pair<JitArtifactKey,
                          std::shared_ptr<const BlockArtifact>>>
        new_artifacts;
    {
      const std::lock_guard lock{mutex_};
      if (disk_source_path_.empty() || disk_source_path_ != path) {
        return AppendResult::NotApplicable;
      }
      if (journal.header_valid) {
        for (const auto &entry : journal.entries) {
          DiskArtifactRecord record{entry.offset, entry.serialized_bytes,
                                    true, {}, false, 0U};
          const auto existing = disk_artifacts_.find(entry.key);
          const bool changed =
              existing == disk_artifacts_.end() ||
              existing->second.offset != record.offset ||
              existing->second.serialized_bytes != record.serialized_bytes ||
              existing->second.append_log != record.append_log;
          if (changed) {
            record.generation = next_disk_generation_locked();
            disk_artifacts_[entry.key] = record;
            disk_order_.push_back(entry.key);
          }
        }
        disk_append_path_ = append_path;
        disk_append_valid_bytes_ = journal.valid_bytes;
      }

      for (auto pending = writeback_order_.begin();
           pending != writeback_order_.end();) {
        if (!disk_artifacts_.contains(*pending)) {
          ++pending;
          continue;
        }
        const auto persisted_key = *pending;
        ++pending;
        retire_writeback_locked(persisted_key);
      }

      new_artifacts.reserve(artifacts_.size() + pending_writebacks_.size());
      std::unordered_set<JitArtifactKey, JitArtifactKeyHash> considered;
      considered.reserve(artifacts_.size() + pending_writebacks_.size());
      const auto consider = [&](const JitArtifactKey &key) {
        if (!considered.insert(key).second) return;
        if (disk_artifacts_.find(key) != disk_artifacts_.end()) return;
        const auto resident = artifacts_.find(key);
        if (resident != artifacts_.end()) {
          new_artifacts.emplace_back(key, resident->second.artifact);
          return;
        }
        const auto pending = pending_writebacks_.find(key);
        if (pending != pending_writebacks_.end()) {
          new_artifacts.emplace_back(key, pending->second.artifact);
        }
      };
      for (const auto &key : lru_) consider(key);
      for (const auto &entry : artifacts_) consider(entry.first);
      for (const auto &key : writeback_order_) consider(key);
      for (const auto &entry : pending_writebacks_) consider(entry.first);
      if (new_artifacts.empty()) return AppendResult::Saved;
      if (new_artifacts.size() > maximum_artifacts ||
          disk_artifacts_.size() >
              maximum_artifacts -
                  static_cast<std::uint32_t>(new_artifacts.size())) {
        return AppendResult::Failed;
      }
    }

    if (!journal.header_valid) {
      const auto parent = append_path.parent_path();
      if (!parent.empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(parent, directory_error);
        if (directory_error) return AppendResult::Failed;
      }
      std::ofstream initialize{append_path,
                               std::ios::binary | std::ios::trunc};
      if (!initialize) return AppendResult::Failed;
      initialize.write(
          artifact_append_magic.data(),
          static_cast<std::streamsize>(artifact_append_magic.size()));
      initialize.flush();
      if (!initialize) return AppendResult::Failed;
      initialize.close();
      journal = scan_artifact_journal(append_path);
      if (!journal.header_valid) return AppendResult::Failed;
    }
    if (journal.valid_bytes < journal.file_size) {
      std::error_code truncate_error;
      std::filesystem::resize_file(append_path, journal.valid_bytes,
                                   truncate_error);
      if (truncate_error) return AppendResult::Failed;
      journal.file_size = journal.valid_bytes;
    }
    if (journal.file_size > maximum_persistence_bytes ||
        journal.file_size > configured_limit) {
      return AppendResult::Failed;
    }
    std::vector<std::uint64_t> record_bytes;
    record_bytes.reserve(new_artifacts.size());
    std::uint64_t segment_bytes = sizeof(std::uint32_t);
    for (const auto &entry : new_artifacts) {
      const auto artifact_size = serialized_artifact_bytes(entry.second->data);
      if (!artifact_size ||
          *artifact_size > std::numeric_limits<std::uint64_t>::max()) {
        return AppendResult::Failed;
      }
      const auto bytes = static_cast<std::uint64_t>(*artifact_size);
      if (bytes > std::numeric_limits<std::uint64_t>::max() - segment_bytes) {
        return AppendResult::Failed;
      }
      segment_bytes += bytes;
      record_bytes.push_back(bytes);
    }
    if (segment_bytes >
        std::numeric_limits<std::uint64_t>::max() - artifact_checksum_bytes) {
      return AppendResult::Failed;
    }
    const auto append_bytes =
        segment_bytes + static_cast<std::uint64_t>(artifact_checksum_bytes);
    if (journal.file_size >
        std::numeric_limits<std::uint64_t>::max() - append_bytes) {
      return AppendResult::Failed;
    }
    const auto journal_end = journal.file_size + append_bytes;
    if (journal_end > maximum_persistence_bytes ||
        journal_end > configured_limit ||
        base_size > configured_limit - journal.file_size ||
        append_bytes > configured_limit - base_size - journal.file_size) {
      return AppendResult::Failed;
    }
    if (!has_storage_headroom(path, limits_.minimum_free_bytes,
                              static_cast<std::uintmax_t>(append_bytes))) {
      return AppendResult::Failed;
    }

    std::ofstream stream{append_path, std::ios::binary | std::ios::app};
    if (!stream) return AppendResult::Failed;
    const auto segment_offset_position = stream.tellp();
    if (segment_offset_position < 0 ||
        static_cast<std::uint64_t>(segment_offset_position) !=
            journal.file_size) {
      return AppendResult::Failed;
    }
    const auto segment_offset = static_cast<std::uint64_t>(
        segment_offset_position);
    write_u32(stream, static_cast<std::uint32_t>(new_artifacts.size()));
    std::uint64_t record_offset = segment_offset + sizeof(std::uint32_t);
    std::vector<DiskArtifactRecord> output_records;
    output_records.reserve(new_artifacts.size());
    for (std::size_t index = 0; index < new_artifacts.size(); ++index) {
      if (!write_artifact(stream, *new_artifacts[index].second)) {
        return AppendResult::Failed;
      }
      output_records.push_back(
          DiskArtifactRecord{record_offset, record_bytes[index], true, {},
                             false, 0U});
      if (record_offset >
          std::numeric_limits<std::uint64_t>::max() - record_bytes[index]) {
        return AppendResult::Failed;
      }
      record_offset += record_bytes[index];
    }
    stream.flush();
    if (!stream || stream.tellp() != static_cast<std::streamoff>(record_offset)) {
      return AppendResult::Failed;
    }
    const auto checksum = sha256_file(
        append_path, segment_offset, record_offset - segment_offset);
    if (!checksum) return AppendResult::Failed;
    stream.write(reinterpret_cast<const char *>(checksum->digest.data()),
                 static_cast<std::streamsize>(checksum->digest.size()));
    stream.flush();
    if (!stream ||
        stream.tellp() != static_cast<std::streamoff>(journal_end)) {
      return AppendResult::Failed;
    }
    stream.close();
    if (!stream) return AppendResult::Failed;

    {
      const std::lock_guard lock{mutex_};
      if (disk_source_path_.empty() || disk_source_path_ != path) {
        return AppendResult::NotApplicable;
      }
      disk_append_path_ = append_path;
      for (std::size_t index = 0; index < new_artifacts.size(); ++index) {
        const auto &key = new_artifacts[index].first;
        output_records[index].generation = next_disk_generation_locked();
        disk_artifacts_[key] = output_records[index];
        disk_order_.push_back(key);
        retire_writeback_locked(key);
      }
      disk_append_valid_bytes_ = journal_end;
    }
    return AppendResult::Saved;
  } catch (...) {
    return AppendResult::Failed;
  }
}

bool JitArtifactStore::save() const noexcept {
  return persistence_path_.empty() || save(persistence_path_);
}

bool JitArtifactStore::save(const std::filesystem::path &path) const noexcept {
  if (path.empty()) return false;
  if (!limits_.persistence_enabled) return true;
  const std::lock_guard persistence_lock{persistence_mutex_};
  auto file_lock = ArtifactFileLock::acquire(
      path, ArtifactFileLock::Mode::Exclusive);
  if (!file_lock) return false;
  const auto writer_generation = file_lock->generation();
  if (!writer_generation) return false;
  bool tracks_path = false;
  std::uint64_t known_generation = 0;
  {
    const std::lock_guard lock{mutex_};
    tracks_path = persistence_path_ == path || disk_source_path_ == path;
    known_generation = external_writer_generation_;
  }
  if (tracks_path && known_generation != *writer_generation) {
    std::error_code exists_error;
    const auto cache_exists = std::filesystem::exists(path, exists_error);
    if (exists_error || (cache_exists && !load_coordinated(path))) {
      return false;
    }
    const std::lock_guard lock{mutex_};
    external_writer_generation_ = *writer_generation;
  }
  const auto next_writer_generation = file_lock->begin_write();
  if (!next_writer_generation) return false;
  {
    const std::lock_guard lock{mutex_};
    external_writer_generation_ = *next_writer_generation;
  }
  const auto append_result = append_new_artifacts(path);
  if (append_result == AppendResult::Saved) return true;
  if (append_result == AppendResult::Failed) return false;
  return save_full(path);
}

bool JitArtifactStore::save_full(
    const std::filesystem::path &path) const noexcept {
  try {
    if (path.empty()) return false;
    struct SaveEntry {
      JitArtifactKey key;
      std::shared_ptr<const BlockArtifact> artifact;
      DiskArtifactRecord disk_record;
      bool resident{};
    };

    std::vector<SaveEntry> entries;
    std::filesystem::path source_path;
    std::filesystem::path append_source_path;
    {
      const std::lock_guard lock{mutex_};
      entries.reserve(disk_artifacts_.size() + artifacts_.size() +
                      pending_writebacks_.size());
      std::unordered_set<JitArtifactKey, JitArtifactKeyHash> emitted;
      emitted.reserve(disk_artifacts_.size() + artifacts_.size() +
                      pending_writebacks_.size());
      const auto append_disk_entry = [&entries, &emitted, this](
                                         const JitArtifactKey &key) {
        const auto disk = disk_artifacts_.find(key);
        if (disk == disk_artifacts_.end() || !emitted.insert(key).second) {
          return;
        }
        const auto resident = artifacts_.find(key);
        if (resident != artifacts_.end()) {
          entries.push_back(
              SaveEntry{key, resident->second.artifact, {}, true});
        } else if (const auto pending = pending_writebacks_.find(key);
                   pending != pending_writebacks_.end()) {
          entries.push_back(
              SaveEntry{key, pending->second.artifact, {}, true});
        } else {
          entries.push_back(SaveEntry{key, {}, disk->second, false});
        }
      };
      for (const auto &key : disk_order_) append_disk_entry(key);
      for (const auto &entry : disk_artifacts_) {
        append_disk_entry(entry.first);
      }
      const auto append_resident_entry = [&entries, &emitted, this](
                                             const JitArtifactKey &key) {
        if (!emitted.insert(key).second) return;
        const auto resident = artifacts_.find(key);
        if (resident != artifacts_.end()) {
          entries.push_back(
              SaveEntry{key, resident->second.artifact, {}, true});
          return;
        }
        const auto pending = pending_writebacks_.find(key);
        if (pending != pending_writebacks_.end()) {
          entries.push_back(
              SaveEntry{key, pending->second.artifact, {}, true});
        }
      };
      for (const auto &key : lru_) append_resident_entry(key);
      for (const auto &entry : artifacts_) append_resident_entry(entry.first);
      for (const auto &key : writeback_order_) append_resident_entry(key);
      for (const auto &entry : pending_writebacks_) {
        append_resident_entry(entry.first);
      }
      source_path = disk_source_path_;
      append_source_path = disk_append_path_;
    }

    if (entries.size() > maximum_artifacts) return false;
    std::vector<std::uint64_t> record_bytes;
    record_bytes.reserve(entries.size());
    std::size_t serialized_size = artifact_magic.size() + sizeof(std::uint32_t);
    bool needs_source = false;
    bool needs_append_source = false;
    for (const auto &entry : entries) {
      std::uint64_t bytes = 0;
      if (entry.resident) {
        const auto artifact_size =
            serialized_artifact_bytes(entry.artifact->data);
        if (!artifact_size ||
            *artifact_size > std::numeric_limits<std::uint64_t>::max()) {
          return false;
        }
        bytes = static_cast<std::uint64_t>(*artifact_size);
      } else {
        bytes = entry.disk_record.serialized_bytes;
        if (entry.disk_record.append_log) {
          needs_append_source = true;
        } else {
          needs_source = true;
        }
      }
      if (bytes > std::numeric_limits<std::size_t>::max() ||
          static_cast<std::size_t>(bytes) >
              std::numeric_limits<std::size_t>::max() - serialized_size) {
        return false;
      }
      serialized_size += static_cast<std::size_t>(bytes);
      record_bytes.push_back(bytes);
    }
    if (entries.size() >
        (std::numeric_limits<std::size_t>::max() -
         artifact_index_header_bytes) /
            artifact_index_entry_bytes) {
      return false;
    }
    const auto index_size = artifact_index_header_bytes +
                            entries.size() * artifact_index_entry_bytes;
    if (serialized_size >
            std::numeric_limits<std::size_t>::max() - index_size ||
        serialized_size + index_size >
            std::numeric_limits<std::size_t>::max() - artifact_footer_bytes) {
      return false;
    }
    const auto total_size =
        serialized_size + index_size + artifact_footer_bytes;
    if (limits_.persistence_bytes != 0U &&
        total_size > limits_.persistence_bytes) {
      return false;
    }
    if (total_size > maximum_persistence_bytes) return false;
    if (!has_storage_headroom(
            path, limits_.minimum_free_bytes,
            static_cast<std::uintmax_t>(total_size))) {
      return false;
    }
    std::filesystem::path new_source_path{path};
    std::ifstream source;
    if (needs_source) {
      if (source_path.empty()) return false;
      source.open(source_path, std::ios::binary);
      if (!source) return false;
    }
    std::ifstream append_source;
    if (needs_append_source) {
      if (append_source_path.empty()) return false;
      append_source.open(append_source_path, std::ios::binary);
      if (!append_source) return false;
    }
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const auto temporary = std::filesystem::path{
        path.string() + ".tmp-" +
        std::to_string(std::hash<std::thread::id>{}(
            std::this_thread::get_id())) +
        "-" + std::to_string(std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())};
    {
      std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
      if (!stream) return false;
      stream.write(artifact_magic.data(),
                   static_cast<std::streamsize>(artifact_magic.size()));
      write_u32(stream, static_cast<std::uint32_t>(entries.size()));
      std::uint64_t output_offset =
          artifact_magic.size() + sizeof(std::uint32_t);
      std::vector<DiskArtifactRecord> output_records;
      output_records.reserve(entries.size());
      for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto &entry = entries[index];
        const auto bytes = record_bytes[index];
        auto &source_stream = entry.disk_record.append_log ? append_source
                                                            : source;
        if (!entry.resident && entry.disk_record.checksum_valid) {
          const auto &record_source = entry.disk_record.append_log
                                          ? append_source_path
                                          : source_path;
          const auto source_checksum = sha256_file(
              record_source, entry.disk_record.offset, bytes);
          if (!source_checksum ||
              *source_checksum != entry.disk_record.checksum) {
            return false;
          }
        }
        const bool written = entry.resident
                                 ? write_artifact(stream, *entry.artifact)
                                 : copy_disk_record(
                                       source_stream, stream,
                                       entry.disk_record.offset, bytes);
        if (!written) return false;
        output_records.push_back(DiskArtifactRecord{
            output_offset, bytes, false, {}, false, 0U});
        if (output_offset > std::numeric_limits<std::uint64_t>::max() -
                                bytes) {
          return false;
        }
        output_offset += bytes;
      }
      stream.flush();
      if (!stream || stream.tellp() != static_cast<std::streamoff>(
                                output_offset)) {
        return false;
      }
      stream.close();
      if (!stream || output_offset != serialized_size) return false;

      for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto checksum = sha256_file(
            temporary, output_records[index].offset,
            output_records[index].serialized_bytes);
        if (!checksum) return false;
        output_records[index].checksum = *checksum;
        output_records[index].checksum_valid = true;
      }

      const auto index_offset = output_offset;
      std::ofstream index_stream{temporary, std::ios::binary | std::ios::app};
      if (!index_stream) return false;
      index_stream.write(
          artifact_index_magic.data(),
          static_cast<std::streamsize>(artifact_index_magic.size()));
      write_u32(index_stream, static_cast<std::uint32_t>(entries.size()));
      for (std::size_t index = 0; index < entries.size(); ++index) {
        write_key(index_stream, entries[index].key);
        write_u64(index_stream, output_records[index].offset);
        write_u64(index_stream, output_records[index].serialized_bytes);
        write_identity(index_stream, output_records[index].checksum);
      }
      index_stream.flush();
      if (!index_stream ||
          index_stream.tellp() != static_cast<std::streamoff>(
                                      index_offset + index_size)) {
        return false;
      }
      index_stream.close();
      if (!index_stream) return false;
      const auto index_checksum = sha256_file(
          temporary, index_offset, static_cast<std::uint64_t>(index_size));
      if (!index_checksum) return false;

      std::ofstream footer_stream{temporary,
                                  std::ios::binary | std::ios::app};
      if (!footer_stream) return false;
      footer_stream.write(
          artifact_footer_magic.data(),
          static_cast<std::streamsize>(artifact_footer_magic.size()));
      write_u64(footer_stream, index_offset);
      write_u64(footer_stream, static_cast<std::uint64_t>(index_size));
      write_u32(footer_stream, static_cast<std::uint32_t>(entries.size()));
      write_identity(footer_stream, *index_checksum);
      footer_stream.flush();
      if (!footer_stream ||
          footer_stream.tellp() != static_cast<std::streamoff>(total_size)) {
        return false;
      }
      footer_stream.close();
      if (!footer_stream) return false;

      DiskArtifactMap output_index;
      output_index.reserve(entries.size());
      std::vector<JitArtifactKey> output_order;
      output_order.reserve(entries.size());
      for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto [iterator, inserted] = output_index.emplace(
            entries[index].key, output_records[index]);
        if (!inserted || iterator->first != entries[index].key) return false;
        output_order.push_back(entries[index].key);
      }

      const std::lock_guard lock{mutex_};
      if (disk_source_path_ != source_path ||
          disk_append_path_ != append_source_path) {
        std::error_code stale_error;
        std::filesystem::remove(temporary, stale_error);
        return false;
      }
      std::error_code error;
      std::filesystem::rename(temporary, path, error);
      if (error) {
        error.clear();
        std::filesystem::remove(temporary, error);
        return false;
      }
      for (auto &entry : output_index) {
        entry.second.generation = next_disk_generation_locked();
      }
      disk_artifacts_ = std::move(output_index);
      disk_order_ = std::move(output_order);
      disk_source_path_ = std::move(new_source_path);
      disk_append_path_ = append_path_for(path);
      disk_append_valid_bytes_ = 0;
      for (const auto &entry : entries) {
        retire_writeback_locked(entry.key);
      }
      std::error_code sidecar_error;
      std::filesystem::remove(disk_append_path_, sidecar_error);
      return true;
    }
  } catch (...) {
    return false;
  }
}

ExecutionContext::ExecutionContext(std::uint32_t process_id)
    : context_id_{next_context_id.fetch_add(1, std::memory_order_relaxed)},
      process_id_{process_id} {}

std::size_t ExecutionContext::create_link_cell() {
  const std::lock_guard lock{mutex_};
  link_cells_.push_back(std::make_unique<LinkCell>());
  return link_cells_.size() - 1U;
}

void ExecutionContext::link(std::size_t cell, std::uint64_t target_token) {
  const std::lock_guard lock{mutex_};
  link_cells_.at(cell)->target_token.store(target_token,
                                           std::memory_order_release);
}

void ExecutionContext::unlink(std::size_t cell) { link(cell, 0); }

std::uint64_t ExecutionContext::linked_target(std::size_t cell) const {
  const std::lock_guard lock{mutex_};
  return link_cells_.at(cell)->target_token.load(std::memory_order_acquire);
}

} // namespace ilemu
