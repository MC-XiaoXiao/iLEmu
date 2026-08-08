#include "ilemu/jit_artifact.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

namespace ilemu {
namespace {

constexpr std::array<char, 8> artifact_magic{
    'i', 'L', 'J', 'A', 'R', 'T', 'F', '6'};
constexpr std::size_t artifact_checksum_bytes = 32U;
constexpr std::uint32_t maximum_artifacts = 1'000'000;
constexpr std::uint32_t maximum_ir_bytes = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_metadata_entries = 1'000'000;
constexpr std::uintmax_t maximum_persistence_bytes =
    std::uintmax_t{4U} * 1024U * 1024U * 1024U;
std::atomic<std::uint64_t> next_context_id{1};

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
  constexpr std::size_t serialized_key_bytes = 132U;
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

[[nodiscard]] std::optional<std::shared_ptr<const BlockArtifact>>
read_artifact_at(const std::filesystem::path &path, std::uint64_t offset,
                 std::uint64_t expected_bytes) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<std::streamoff>::max()) ||
      expected_bytes > std::numeric_limits<std::uint64_t>::max() - offset) {
    return std::nullopt;
  }
  std::ifstream stream{path, std::ios::binary};
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
  if (!persistence_path_.empty()) static_cast<void>(load(persistence_path_));
}

JitArtifactStore::~JitArtifactStore() { static_cast<void>(save()); }

std::shared_ptr<const BlockArtifact> JitArtifactStore::find(
    const JitArtifactKey &key) const {
  const std::lock_guard lock{mutex_};
  ++stats_.lookups;
  auto artifact = artifacts_.find(key);
  if (artifact == artifacts_.end()) {
    const auto disk_artifact = disk_artifacts_.find(key);
    if (disk_artifact != disk_artifacts_.end() &&
        disk_artifact->second.serialized_bytes <=
            std::numeric_limits<std::size_t>::max()) {
      const auto loaded = read_artifact_at(
          disk_source_path_, disk_artifact->second.offset,
          disk_artifact->second.serialized_bytes);
      if (loaded && (*loaded)->key == key) {
        insert_locked(*loaded,
                      static_cast<std::size_t>(
                          disk_artifact->second.serialized_bytes),
                      true);
        artifact = artifacts_.find(key);
      }
    }
    if (artifact == artifacts_.end()) {
      ++stats_.misses;
      return nullptr;
    }
  }
  if (artifact->second.loaded_from_disk) {
    ++stats_.disk_hits;
    artifact->second.loaded_from_disk = false;
  } else {
    ++stats_.memory_hits;
  }
  touch_locked(artifact);
  return artifact->second.artifact;
}

void JitArtifactStore::touch_locked(ArtifactMap::iterator iterator) const {
  lru_.splice(lru_.end(), lru_, iterator->second.lru_position);
  iterator->second.lru_position = std::prev(lru_.end());
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

std::shared_ptr<const BlockArtifact> JitArtifactStore::publish(
    JitArtifactKey key, JitArtifactData data) {
  const auto artifact_bytes = serialized_artifact_bytes(data);
  if (!artifact_bytes) return nullptr;
  const std::lock_guard lock{mutex_};
  ++stats_.publish_calls;
  if (const auto existing = artifacts_.find(key); existing != artifacts_.end()) {
    ++stats_.deduplicated_publishes;
    touch_locked(existing);
    return existing->second.artifact;
  }
  auto artifact = std::make_shared<const BlockArtifact>(
      BlockArtifact{std::move(key), std::move(data)});
  const auto lookup_key = artifact->key;
  insert_locked(std::move(artifact), *artifact_bytes);
  const auto inserted = artifacts_.find(lookup_key);
  return inserted == artifacts_.end() ? nullptr : inserted->second.artifact;
}

std::size_t JitArtifactStore::size() const {
  const std::lock_guard lock{mutex_};
  std::size_t result = disk_artifacts_.size();
  for (const auto &artifact : artifacts_) {
    if (disk_artifacts_.find(artifact.first) == disk_artifacts_.end()) {
      ++result;
    }
  }
  return result;
}

JitArtifactStoreStats JitArtifactStore::stats() const {
  const std::lock_guard lock{mutex_};
  auto result = stats_;
  result.resident_bytes = resident_bytes_;
  const auto &disk_path = !persistence_path_.empty()
                              ? persistence_path_
                              : disk_source_path_;
  if (!disk_path.empty()) {
    std::error_code error;
    result.disk_bytes = std::filesystem::file_size(disk_path, error);
    if (error) result.disk_bytes = 0;
  }
  return result;
}

bool JitArtifactStore::load(const std::filesystem::path &path) noexcept {
  try {
    struct DiskEntry {
      JitArtifactKey key;
      DiskArtifactRecord record;
    };

    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size < artifact_magic.size() + sizeof(std::uint32_t) +
                                   artifact_checksum_bytes) {
      return false;
    }
    const auto configured_limit = limits_.persistence_bytes == 0U
                                      ? maximum_persistence_bytes
                                      : std::min<std::uintmax_t>(
                                            limits_.persistence_bytes,
                                            maximum_persistence_bytes);
    if (file_size > configured_limit) return false;
    if (file_size > std::numeric_limits<std::uint64_t>::max()) return false;
    const auto payload_size = static_cast<std::uint64_t>(
        file_size - artifact_checksum_bytes);
    std::ifstream stream{path, std::ios::binary};
    if (!stream) return false;
    std::array<char, artifact_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    // F1 stored text DumpBlock output and has no safe portable-IR validation
    // boundary. Reject it instead of retaining an artifact that can never be
    // imported safely. Unknown containers must take the same ordinary-JIT
    // fallback path.
    if (!stream || magic != artifact_magic) {
      return false;
    }
    const auto count = read_u32(stream);
    if (!count || *count > maximum_artifacts) return false;

    DiskArtifactMap loaded_index;
    loaded_index.reserve(*count);
    std::vector<DiskEntry> scanned;
    scanned.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
      const auto record_offset = stream.tellg();
      if (record_offset < 0) return false;
      const auto key = read_key(stream);
      const auto ir_size = read_u32(stream);
      const auto relocation_count = read_u32(stream);
      const auto exit_count = read_u32(stream);
      const auto dependency_count = read_u32(stream);
      const auto constant_dependency_count = read_u32(stream);
      const auto instruction_count = read_u32(stream);
      const auto translation_nanoseconds = read_u64(stream);
      if (!key || !ir_size || !relocation_count || !exit_count ||
          !dependency_count || !instruction_count ||
          !constant_dependency_count || !translation_nanoseconds ||
          *relocation_count > maximum_metadata_entries ||
          *exit_count > maximum_metadata_entries ||
          *dependency_count > maximum_metadata_entries ||
          *constant_dependency_count > maximum_metadata_entries) {
        return false;
      }
      const auto record_bytes = serialized_artifact_bytes(
          *ir_size, *relocation_count, *exit_count, *dependency_count,
          *constant_dependency_count);
      if (!record_bytes ||
          *record_bytes > std::numeric_limits<std::uint64_t>::max()) {
        return false;
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
        return false;
      }
      for (std::uint32_t dependency = 0; dependency < *dependency_count;
           ++dependency) {
        const auto address = read_u32(stream);
        const auto size = read_u32(stream);
        if (!address || !size || *size == 0 ||
            !skip_bytes(stream, ContentIdentity{}.digest.size(),
                        payload_size) ||
            !skip_bytes(stream, ContentIdentity{}.digest.size(),
                        payload_size)) {
          return false;
        }
      }
      for (std::uint32_t dependency = 0;
           dependency < *constant_dependency_count; ++dependency) {
        const auto address = read_u32(stream);
        const auto size = read_u32(stream);
        const auto value = read_u64(stream);
        if (!address || !size || *size == 0 || !value ||
            !skip_bytes(stream, ContentIdentity{}.digest.size(),
                        payload_size) ||
            !skip_bytes(stream, ContentIdentity{}.digest.size(),
                        payload_size)) {
          return false;
        }
      }
      const auto end = stream.tellg();
      if (end < 0 || static_cast<std::uint64_t>(end) <
                          static_cast<std::uint64_t>(record_offset) ||
          static_cast<std::uint64_t>(end) -
                  static_cast<std::uint64_t>(record_offset) != *record_bytes) {
        return false;
      }
      const auto offset = static_cast<std::uint64_t>(record_offset);
      const DiskArtifactRecord record{offset,
                                      static_cast<std::uint64_t>(*record_bytes)};
      loaded_index[*key] = record;
      scanned.push_back(DiskEntry{*key, record});
    }
    if (stream.tellg() != static_cast<std::streamoff>(payload_size)) {
      return false;
    }
    ContentIdentity expected_checksum;
    stream.read(reinterpret_cast<char *>(expected_checksum.digest.data()),
                static_cast<std::streamsize>(expected_checksum.digest.size()));
    if (!stream) return false;
    const auto actual_checksum = sha256_file(
        path, 0, static_cast<std::uint64_t>(payload_size));
    if (!actual_checksum || *actual_checksum != expected_checksum) {
      return false;
    }

    std::vector<DiskEntry> unique;
    unique.reserve(scanned.size());
    for (const auto &entry : scanned) {
      const auto latest = loaded_index.find(entry.key);
      if (latest != loaded_index.end() &&
          latest->second.offset == entry.record.offset) {
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
      const auto artifact = read_artifact_at(
          path, entry.record.offset, entry.record.serialized_bytes);
      if (!artifact || (*artifact)->key != entry.key) return false;
      loaded.push_back(*artifact);
    }
    std::vector<JitArtifactKey> loaded_order;
    loaded_order.reserve(unique.size());
    for (const auto &entry : unique) loaded_order.push_back(entry.key);
    std::filesystem::path loaded_source_path{path};

    const std::lock_guard lock{mutex_};
    disk_artifacts_ = std::move(loaded_index);
    disk_order_ = std::move(loaded_order);
    disk_source_path_ = std::move(loaded_source_path);
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

bool JitArtifactStore::save() const noexcept {
  return persistence_path_.empty() || save(persistence_path_);
}

bool JitArtifactStore::save(const std::filesystem::path &path) const noexcept {
  try {
    if (path.empty()) return false;
    struct SaveEntry {
      JitArtifactKey key;
      std::shared_ptr<const BlockArtifact> artifact;
      DiskArtifactRecord disk_record;
      bool resident{};
    };

    const std::lock_guard lock{mutex_};
    std::vector<SaveEntry> entries;
    entries.reserve(disk_artifacts_.size() + artifacts_.size());
    std::unordered_set<JitArtifactKey, JitArtifactKeyHash> emitted;
    emitted.reserve(disk_artifacts_.size() + artifacts_.size());
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
      }
    };
    for (const auto &key : lru_) append_resident_entry(key);
    for (const auto &entry : artifacts_) append_resident_entry(entry.first);

    if (entries.size() > maximum_artifacts) return false;
    std::vector<std::uint64_t> record_bytes;
    record_bytes.reserve(entries.size());
    std::size_t serialized_size = artifact_magic.size() + sizeof(std::uint32_t);
    bool needs_source = false;
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
        needs_source = true;
      }
      if (bytes > std::numeric_limits<std::size_t>::max() ||
          static_cast<std::size_t>(bytes) >
              std::numeric_limits<std::size_t>::max() - serialized_size) {
        return false;
      }
      serialized_size += static_cast<std::size_t>(bytes);
      record_bytes.push_back(bytes);
    }
    if (serialized_size >
        std::numeric_limits<std::size_t>::max() - artifact_checksum_bytes) {
      return false;
    }
    const auto total_size = serialized_size + artifact_checksum_bytes;
    if (limits_.persistence_bytes != 0U &&
        total_size > limits_.persistence_bytes) {
      return false;
    }
    if (total_size > maximum_persistence_bytes) return false;
    std::filesystem::path new_source_path{path};
    std::ifstream source;
    if (needs_source) {
      if (disk_source_path_.empty()) return false;
      source.open(disk_source_path_, std::ios::binary);
      if (!source) return false;
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
        const bool written = entry.resident
                                 ? write_artifact(stream, *entry.artifact)
                                 : copy_disk_record(
                                       source, stream,
                                       entry.disk_record.offset, bytes);
        if (!written) return false;
        output_records.push_back(DiskArtifactRecord{output_offset, bytes});
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

      const auto checksum = sha256_file(
          temporary, 0, static_cast<std::uint64_t>(serialized_size));
      if (!checksum) return false;
      stream.close();
      std::ofstream checksum_stream{temporary,
                                    std::ios::binary | std::ios::app};
      if (!checksum_stream) return false;
      checksum_stream.write(
          reinterpret_cast<const char *>(checksum->digest.data()),
          static_cast<std::streamsize>(checksum->digest.size()));
      checksum_stream.flush();
      if (!checksum_stream) return false;

      std::error_code error;
      std::filesystem::rename(temporary, path, error);
      if (error) {
        error.clear();
        std::filesystem::remove(temporary, error);
        return false;
      }
      disk_artifacts_ = std::move(output_index);
      disk_order_ = std::move(output_order);
      disk_source_path_ = std::move(new_source_path);
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
