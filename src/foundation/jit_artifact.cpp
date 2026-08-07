#include "ilemu/jit_artifact.hpp"

#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace ilemu {
namespace {

constexpr std::array<char, 8> artifact_magic{
    'i', 'L', 'J', 'A', 'R', 'T', 'F', '1'};
constexpr std::uint32_t maximum_artifacts = 1'000'000;
constexpr std::uint32_t maximum_ir_bytes = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_metadata_entries = 1'000'000;
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
  stream.put(static_cast<char>(key.architecture));
  stream.put(static_cast<char>(key.cpu_model));
  stream.put('\0');
  write_u32(stream, key.timing_model_version);
  write_u32(stream, key.guest_ticks_per_second);
  write_u32(stream, key.image_slide);
  write_u32(stream, key.hle_abi_version);
  write_u32(stream, key.backend_abi_version);
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
  const auto architecture = stream.get();
  const auto cpu_model = stream.get();
  if (thumb == std::char_traits<char>::eof() ||
      architecture == std::char_traits<char>::eof() ||
      cpu_model == std::char_traits<char>::eof() ||
      stream.get() == std::char_traits<char>::eof()) {
    return std::nullopt;
  }
  key.thumb = thumb != 0;
  key.architecture = static_cast<ArmArchitectureVersion>(architecture);
  key.cpu_model = static_cast<ArmCpuModelKind>(cpu_model);
  const auto timing = read_u32(stream);
  const auto ticks = read_u32(stream);
  const auto slide = read_u32(stream);
  const auto hle = read_u32(stream);
  const auto backend = read_u32(stream);
  const auto options = read_u64(stream);
  if (!timing || !ticks || !slide || !hle || !backend || !options) {
    return std::nullopt;
  }
  key.timing_model_version = *timing;
  key.guest_ticks_per_second = *ticks;
  key.image_slide = *slide;
  key.hle_abi_version = *hle;
  key.backend_abi_version = *backend;
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

} // namespace

std::size_t JitArtifactKeyHash::operator()(
    const JitArtifactKey &key) const noexcept {
  auto hash = std::size_t{0};
  hash_identity(hash, key.content_identity);
  hash_identity(hash, key.layout_identity);
  hash_scalar(hash, key.guest_pc);
  hash_scalar(hash, key.thumb);
  hash_scalar(hash, key.architecture);
  hash_scalar(hash, key.cpu_model);
  hash_scalar(hash, key.timing_model_version);
  hash_scalar(hash, key.guest_ticks_per_second);
  hash_scalar(hash, key.image_slide);
  hash_scalar(hash, key.hle_abi_version);
  hash_scalar(hash, key.backend_abi_version);
  hash_scalar(hash, key.codegen_options);
  hash_scalar(hash, key.host_isa);
  hash_scalar(hash, key.host_feature_mask);
  hash_scalar(hash, key.artifact_format_version);
  return hash;
}

JitArtifactStore::JitArtifactStore(std::filesystem::path persistence_path)
    : persistence_path_{std::move(persistence_path)} {
  if (!persistence_path_.empty()) static_cast<void>(load(persistence_path_));
}

JitArtifactStore::~JitArtifactStore() { static_cast<void>(save()); }

std::shared_ptr<const BlockArtifact> JitArtifactStore::find(
    const JitArtifactKey &key) const {
  const std::lock_guard lock{mutex_};
  const auto artifact = artifacts_.find(key);
  return artifact == artifacts_.end() ? nullptr : artifact->second;
}

std::shared_ptr<const BlockArtifact> JitArtifactStore::publish(
    JitArtifactKey key, JitArtifactData data) {
  const std::lock_guard lock{mutex_};
  if (const auto existing = artifacts_.find(key); existing != artifacts_.end()) {
    return existing->second;
  }
  auto artifact = std::make_shared<const BlockArtifact>(
      BlockArtifact{std::move(key), std::move(data)});
  artifacts_.emplace(artifact->key, artifact);
  return artifact;
}

std::size_t JitArtifactStore::size() const {
  const std::lock_guard lock{mutex_};
  return artifacts_.size();
}

bool JitArtifactStore::load(const std::filesystem::path &path) noexcept {
  try {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) return false;
    std::array<char, artifact_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!stream || magic != artifact_magic) return false;
    const auto count = read_u32(stream);
    if (!count || *count > maximum_artifacts) return false;

    std::vector<std::shared_ptr<const BlockArtifact>> loaded;
    loaded.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
      const auto key = read_key(stream);
      const auto ir_size = read_u32(stream);
      const auto relocation_count = read_u32(stream);
      const auto exit_count = read_u32(stream);
      const auto instruction_count = read_u32(stream);
      if (!key || !ir_size || !relocation_count || !exit_count ||
          !instruction_count || *relocation_count > maximum_metadata_entries ||
          *exit_count > maximum_metadata_entries) {
        return false;
      }
      const auto ir = read_bytes(stream, *ir_size);
      if (!ir) return false;
      JitArtifactData data;
      data.normalized_ir = *ir;
      data.instruction_count = *instruction_count;
      data.relocation_targets.reserve(*relocation_count);
      for (std::uint32_t relocation = 0; relocation < *relocation_count;
           ++relocation) {
        const auto target = read_u64(stream);
        if (!target) return false;
        data.relocation_targets.push_back(*target);
      }
      data.exit_locations.reserve(*exit_count);
      for (std::uint32_t exit = 0; exit < *exit_count; ++exit) {
        const auto location = read_u64(stream);
        if (!location) return false;
        data.exit_locations.push_back(*location);
      }
      loaded.push_back(std::make_shared<const BlockArtifact>(
          BlockArtifact{*key, std::move(data)}));
    }
    if (stream.peek() != std::char_traits<char>::eof()) return false;

    const std::lock_guard lock{mutex_};
    for (auto &artifact : loaded) {
      artifacts_.try_emplace(artifact->key, std::move(artifact));
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
    std::vector<std::shared_ptr<const BlockArtifact>> snapshot;
    {
      const std::lock_guard lock{mutex_};
      snapshot.reserve(artifacts_.size());
      for (const auto &[key, artifact] : artifacts_) {
        static_cast<void>(key);
        snapshot.push_back(artifact);
      }
    }
    if (snapshot.size() > maximum_artifacts) return false;
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const auto temporary = path.string() + ".tmp";
    {
      std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
      if (!stream) return false;
      stream.write(artifact_magic.data(),
                   static_cast<std::streamsize>(artifact_magic.size()));
      write_u32(stream, static_cast<std::uint32_t>(snapshot.size()));
      for (const auto &artifact : snapshot) {
        if (artifact->data.normalized_ir.size() > maximum_ir_bytes ||
            artifact->data.relocation_targets.size() >
                maximum_metadata_entries ||
            artifact->data.exit_locations.size() > maximum_metadata_entries) {
          return false;
        }
        write_key(stream, artifact->key);
        write_u32(stream, static_cast<std::uint32_t>(
                              artifact->data.normalized_ir.size()));
        write_u32(stream, static_cast<std::uint32_t>(
                              artifact->data.relocation_targets.size()));
        write_u32(stream, static_cast<std::uint32_t>(
                              artifact->data.exit_locations.size()));
        write_u32(stream, artifact->data.instruction_count);
        stream.write(
            reinterpret_cast<const char *>(artifact->data.normalized_ir.data()),
            static_cast<std::streamsize>(artifact->data.normalized_ir.size()));
        for (const auto target : artifact->data.relocation_targets)
          write_u64(stream, target);
        for (const auto location : artifact->data.exit_locations)
          write_u64(stream, location);
      }
      stream.flush();
      if (!stream) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
      error.clear();
      std::filesystem::remove(temporary, error);
      return false;
    }
    return true;
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
