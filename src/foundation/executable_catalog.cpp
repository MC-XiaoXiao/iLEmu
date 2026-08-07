#include "ilemu/executable_catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <string_view>

namespace {

using ilemu::ContentIdentity;
using ilemu::DyldCacheImage;
using ilemu::DyldSharedCache;

void append_u32(std::vector<std::byte> &bytes, std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
  }
}

void append_u64(std::vector<std::byte> &bytes, std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
  }
}

void append_string(std::vector<std::byte> &bytes, std::string_view value) {
  append_u64(bytes, static_cast<std::uint64_t>(value.size()));
  bytes.insert(bytes.end(),
               reinterpret_cast<const std::byte *>(value.data()),
               reinterpret_cast<const std::byte *>(value.data() + value.size()));
}

void append_identity(std::vector<std::byte> &bytes,
                     const ContentIdentity &identity) {
  bytes.insert(bytes.end(), identity.digest.begin(), identity.digest.end());
}

ContentIdentity shared_cache_image_identity(const DyldSharedCache &cache,
                                            const DyldCacheImage &image) {
  std::vector<std::byte> key;
  key.reserve(128U + image.path.size() +
              image.executable_ranges.size() * 40U);
  append_identity(key, cache.generation_identity());
  append_u32(key, image.index);
  append_string(key, image.path);
  append_u64(key, image.unslid_load_address);
  append_u64(key, image.text_segment_size);
  key.push_back(static_cast<std::byte>(image.text_uuid.has_value()));
  if (image.text_uuid) {
    key.insert(key.end(), image.text_uuid->begin(), image.text_uuid->end());
  }
  key.push_back(static_cast<std::byte>(image.text_identity.has_value()));
  if (image.text_identity) append_identity(key, *image.text_identity);
  append_u64(key, static_cast<std::uint64_t>(image.executable_ranges.size()));
  for (const auto &range : image.executable_ranges) {
    append_u64(key, range.address);
    append_u64(key, range.size);
    append_u64(key, range.file_offset);
    append_u32(key, range.file_index);
    append_u32(key, range.initial_protection);
    append_u32(key, range.maximum_protection);
  }
  return ilemu::sha256(key);
}

ilemu::ExecutableCatalogKind classify_shared_cache_image(
    std::string_view path) {
  if (path.find(".framework/") != std::string_view::npos ||
      path.ends_with(".framework")) {
    return ilemu::ExecutableCatalogKind::Framework;
  }
  if (path.find("/PlugIns/") != std::string_view::npos ||
      path.ends_with(".plugin") || path.ends_with(".appex")) {
    return ilemu::ExecutableCatalogKind::PlugIn;
  }
  return ilemu::ExecutableCatalogKind::Dylib;
}

} // namespace

namespace ilemu {

const ExecutableCatalogEntry &ExecutableCatalog::register_image(
    const MachOImage &image) {
  const auto normalized = normalize_path(image.path());
  auto &entry = upsert(image.content_identity(), normalized,
                       classify(image, normalized));
  entry.uuid = image.uuid();
  entry.cpu_type = image.cpu_type();
  entry.cpu_subtype = image.cpu_subtype();
  entry.file_type = image.file_type();
  entry.fat_container = image.fat_container();
  for (const auto &dylib : image.dylibs()) {
    if (std::find(entry.dependencies.begin(), entry.dependencies.end(),
                  dylib.path) == entry.dependencies.end()) {
      entry.dependencies.push_back(dylib.path);
    }
  }
  return entry;
}

const ExecutableCatalogEntry &ExecutableCatalog::register_path(
    const std::filesystem::path &path, ArmArchitectureVersion architecture) {
  return register_image(MachOImage::parse(path, architecture));
}

const ExecutableCatalogEntry &ExecutableCatalog::register_mapping(
    const std::filesystem::path &path, std::uint64_t file_offset,
    std::uint64_t byte_count) {
  const auto identity = sha256_file(path);
  if (!identity) {
    throw std::runtime_error{"cannot hash dynamic executable mapping: " +
                             path.string()};
  }
  auto &entry = upsert(*identity, normalize_path(path),
                       ExecutableCatalogKind::DynamicMapping);
  const auto mapping = ExecutableMappingIdentity{file_offset, byte_count};
  if (std::find(entry.mappings.begin(), entry.mappings.end(), mapping) ==
      entry.mappings.end()) {
    entry.mappings.push_back(mapping);
  }
  return entry;
}

std::size_t ExecutableCatalog::register_shared_cache(
    const DyldSharedCache &cache) {
  std::size_t registered = 0;
  for (const auto &image : cache.images()) {
    auto &entry = upsert(shared_cache_image_identity(cache, image),
                         std::filesystem::path{image.path},
                         classify_shared_cache_image(image.path));
    entry.uuid = image.text_uuid;
    entry.file_type = 6U; // MH_DYLIB: shared-cache images are dylib code.
    for (const auto &range : image.executable_ranges) {
      const auto mapping =
          ExecutableMappingIdentity{range.file_offset, range.size};
      if (std::find(entry.mappings.begin(), entry.mappings.end(), mapping) ==
          entry.mappings.end()) {
        entry.mappings.push_back(mapping);
      }
      if (range.file_index < cache.files().size()) {
        const auto &file_path = cache.files()[range.file_index].path;
        if (std::find(entry.aliases.begin(), entry.aliases.end(), file_path) ==
            entry.aliases.end()) {
          entry.aliases.push_back(file_path);
        }
      }
    }
    ++registered;
  }
  return registered;
}

const ExecutableCatalogEntry *ExecutableCatalog::find(
    const ContentIdentity &identity) const {
  const auto iterator = identity_index_.find(identity);
  return iterator == identity_index_.end() ? nullptr
                                            : &entries_[iterator->second];
}

std::filesystem::path ExecutableCatalog::normalize_path(
    const std::filesystem::path &path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(path, error);
  }
  return (error ? path : normalized).lexically_normal();
}

ExecutableCatalogKind ExecutableCatalog::classify(
    const MachOImage &image, const std::filesystem::path &path) {
  for (const auto &component : path) {
    const auto text = component.string();
    if (text.ends_with(".framework")) return ExecutableCatalogKind::Framework;
    if (text == "PlugIns" || text.ends_with(".plugin") ||
        text.ends_with(".appex")) {
      return ExecutableCatalogKind::PlugIn;
    }
  }
  if (image.file_type() == 6U) return ExecutableCatalogKind::Dylib;
  return ExecutableCatalogKind::MachO;
}

ExecutableCatalogEntry &ExecutableCatalog::upsert(
    ContentIdentity identity, const std::filesystem::path &path,
    ExecutableCatalogKind kind) {
  const auto existing = identity_index_.find(identity);
  if (existing == identity_index_.end()) {
    const auto index = entries_.size();
    entries_.push_back(ExecutableCatalogEntry{
        .content_identity = identity,
        .aliases = {path},
        .kinds = {kind},
        .uuid = std::nullopt,
        .cpu_type = 0U,
        .cpu_subtype = 0U,
        .file_type = 0U,
        .fat_container = false,
        .dependencies = {},
        .mappings = {},
    });
    identity_index_.emplace(std::move(identity), index);
    return entries_.back();
  }
  auto &entry = entries_[existing->second];
  if (std::find(entry.aliases.begin(), entry.aliases.end(), path) ==
      entry.aliases.end()) {
    entry.aliases.push_back(path);
  }
  entry.kinds.insert(kind);
  return entry;
}

} // namespace ilemu
