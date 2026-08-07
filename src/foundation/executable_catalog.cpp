#include "ilemu/executable_catalog.hpp"

#include <algorithm>
#include <stdexcept>
#include <system_error>

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
