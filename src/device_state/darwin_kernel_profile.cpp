#include "ilemu/darwin_kernel_profile.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

#if defined(ILEMU_HAS_LIBPLIST)
#include <plist/plist.h>
#endif

namespace ilemu {
namespace {

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input{path, std::ios::binary};
  if (!input)
    return {};
  return {std::istreambuf_iterator<char>{input},
          std::istreambuf_iterator<char>{}};
}

std::optional<std::string> xml_string(std::string_view xml,
                                      std::string_view key) {
  const auto encoded_key = "<key>" + std::string{key} + "</key>";
  const auto key_position = xml.find(encoded_key);
  if (key_position == std::string_view::npos)
    return std::nullopt;
  constexpr std::string_view opening{"<string>"};
  constexpr std::string_view closing{"</string>"};
  const auto value_position =
      xml.find(opening, key_position + encoded_key.size());
  if (value_position == std::string_view::npos)
    return std::nullopt;
  const auto value_begin = value_position + opening.size();
  const auto value_end = xml.find(closing, value_begin);
  if (value_end == std::string_view::npos)
    return std::nullopt;
  return std::string{xml.substr(value_begin, value_end - value_begin)};
}

struct SystemVersion {
  std::string build_version;
};

DarwinGuestCapabilities capabilities_for_build(std::string_view build) {
  // Local XNU 792 defines slot 322 as nosys. Public xnu-1228 keeps the old
  // disk-only iopolicysys contract, while later xnu-4903 adds policy types
  // and values that are intentionally not exposed through this profile.
  if (build == "1A543a" || build == "3A109a") {
    return DarwinGuestCapabilities{DarwinAbiEpoch::IphoneOs1};
  }
  if (build == "5A347") {
    return DarwinGuestCapabilities{DarwinAbiEpoch::IphoneOs2};
  }
  if (build == "7A341") {
    return DarwinGuestCapabilities{DarwinAbiEpoch::IphoneOs3};
  }
  return {};
}

SystemVersion read_system_version(const std::filesystem::path &rootfs) {
  const auto bytes =
      read_file(rootfs / "System/Library/CoreServices/SystemVersion.plist");
  SystemVersion result;
#if defined(ILEMU_HAS_LIBPLIST)
  plist_t parsed = nullptr;
  plist_format_t format = PLIST_FORMAT_NONE;
  if (!bytes.empty() &&
      plist_from_memory(bytes.data(), static_cast<std::uint32_t>(bytes.size()),
                        &parsed, &format) == PLIST_ERR_SUCCESS &&
      parsed != nullptr && plist_get_node_type(parsed) == PLIST_DICT) {
    const auto read_string = [parsed](const char *key) {
      const auto node = plist_dict_get_item(parsed, key);
      if (node == nullptr || plist_get_node_type(node) != PLIST_STRING)
        return std::string{};
      std::uint64_t length{};
      const auto *value = plist_get_string_ptr(node, &length);
      return value == nullptr
                 ? std::string{}
                 : std::string{value, static_cast<std::size_t>(length)};
    };
    result.build_version = read_string("ProductBuildVersion");
  }
  if (parsed != nullptr)
    plist_free(parsed);
#endif
  if (result.build_version.empty()) {
    result.build_version =
        xml_string(bytes, "ProductBuildVersion").value_or("");
  }
  return result;
}

} // namespace

DarwinKernelIdentityProfile
make_darwin_kernel_identity_profile(const std::filesystem::path &rootfs) {
  const auto system_version = read_system_version(rootfs);
  DarwinKernelIdentityProfile profile;
  if (!system_version.build_version.empty())
    profile.build_version = system_version.build_version;
  profile.capabilities = capabilities_for_build(profile.build_version);
  return profile;
}

} // namespace ilemu
