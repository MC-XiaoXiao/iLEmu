#include "ilemu/darwin_kernel_profile.hpp"

#include <array>
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

enum class BuildMatchKind : std::uint8_t { FamilyPrefix };

struct BuildProfileRule {
  std::string_view matcher;
  BuildMatchKind match_kind;
  DarwinAbiEpoch abi_epoch;
  DarwinPthreadAbiProfile pthread_abi;
  DarwinApple80211IoctlProfile apple80211_ioctl;
  DarwinNotifyStateProfile notify_state_profile;
  DarwinGuestCapabilities capabilities;
  std::string_view profile_name;
  std::string_view operating_system_release;
  std::uint32_t operating_system_revision{};
  std::string_view kernel_version;
};

constexpr BuildProfileRule
build_family_rule(std::string_view matcher, DarwinAbiEpoch abi_epoch,
                  DarwinPthreadAbiProfile pthread_abi,
                  DarwinGuestCapabilities capabilities) {
  return {matcher, BuildMatchKind::FamilyPrefix, abi_epoch, pthread_abi,
          DarwinApple80211IoctlProfile::AlignedCurrentNetworkRecord,
          DarwinNotifyStateProfile::NativeServerTokens, capabilities,
          {}, {}, 0, {}};
}

// Keep build recognition data-driven at the ABI-family boundary. Individual
// firmware releases within an audited family share the same contract; the
// dispatchers consume only the resulting epoch and capabilities. The final
// rule is an audited build-series prefix from the later XNU source set (for
// example 11A465 from xnu-4903), not a catch-all for arbitrary numeric builds.
constexpr std::array build_profile_rules{
    build_family_rule("1A", DarwinAbiEpoch::IphoneOs1,
                      DarwinPthreadAbiProfile::LegacyMachThreads,
                      {true, true, true}),
    build_family_rule("3A", DarwinAbiEpoch::IphoneOs1,
                      DarwinPthreadAbiProfile::LegacyMachThreads,
                      {true, true, true}),
    build_family_rule("4B", DarwinAbiEpoch::IphoneOs1,
                      DarwinPthreadAbiProfile::LegacyMachThreads,
                      {true, true, true}),
    build_family_rule("5A", DarwinAbiEpoch::IphoneOs2,
                      DarwinPthreadAbiProfile::LegacyMachThreads,
                      {true, false, false}),
    build_family_rule("5G", DarwinAbiEpoch::IphoneOs2,
                      DarwinPthreadAbiProfile::LegacyMachThreads,
                      {true, false, false}),
    build_family_rule("7A", DarwinAbiEpoch::IphoneOs3,
                      DarwinPthreadAbiProfile::LegacyMachThreads,
                      {true, false, false}),
    BuildProfileRule{
        "7B", BuildMatchKind::FamilyPrefix, DarwinAbiEpoch::Darwin10,
        DarwinPthreadAbiProfile::BsdThreadRegisterV1,
        DarwinApple80211IoctlProfile::CompactCurrentNetworkRecord,
        DarwinNotifyStateProfile::BootstrapAwareServerTokens,
        {true, false, false}, "darwin10.3-arm", "10.3.1", 199506,
        "Darwin Kernel Version 10.3.1: iLEmu compatibility kernel; "
        "darwin10.3/RELEASE_ARM"},
    build_family_rule("11", DarwinAbiEpoch::Later,
                      DarwinPthreadAbiProfile::BsdThreadRegisterV2,
                      {true, false, false}),
};

[[nodiscard]] bool matches_build(const BuildProfileRule &rule,
                                 std::string_view build) {
  switch (rule.match_kind) {
  case BuildMatchKind::FamilyPrefix:
    return build.starts_with(rule.matcher);
  }
  return false;
}

struct DarwinBuildContract {
  DarwinAbiEpoch abi_epoch{DarwinAbiEpoch::Unknown};
  DarwinPthreadAbiProfile pthread_abi{
      DarwinPthreadAbiProfile::LegacyMachThreads};
  DarwinApple80211IoctlProfile apple80211_ioctl{
      DarwinApple80211IoctlProfile::AlignedCurrentNetworkRecord};
  DarwinNotifyStateProfile notify_state_profile{
      DarwinNotifyStateProfile::NativeServerTokens};
  DarwinGuestCapabilities capabilities{};
  std::string_view profile_name;
  std::string_view operating_system_release;
  std::uint32_t operating_system_revision{};
  std::string_view kernel_version;
};

[[nodiscard]] DarwinBuildContract contract_for_build(std::string_view build) {
  for (const auto &rule : build_profile_rules) {
    if (matches_build(rule, build))
      return {rule.abi_epoch,
              rule.pthread_abi,
              rule.apple80211_ioctl,
              rule.notify_state_profile,
              rule.capabilities,
              rule.profile_name,
              rule.operating_system_release,
              rule.operating_system_revision,
              rule.kernel_version};
  }
  // Unknown epochs intentionally expose no version-sensitive capability.
  // Additive and shape-dispatched routes remain available through their
  // route metadata, while ambiguous calls receive deterministic safe errors.
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
  if (!system_version.build_version.empty()) {
    profile.build_version = system_version.build_version;
    profile.abi_build_version = system_version.build_version;
  } else if (rootfs.empty() || !std::filesystem::exists(rootfs)) {
    // Unit and embedding callers may intentionally omit a firmware rootfs.
    // In that case use the explicitly compiled compatibility default rather
    // than treating the absence of a fixture as an unidentified firmware.
    profile.abi_build_version = profile.build_version;
  }
  const auto contract = contract_for_build(profile.abi_build_version);
  profile.abi_epoch = contract.abi_epoch;
  profile.pthread_abi = contract.pthread_abi;
  profile.apple80211_ioctl = contract.apple80211_ioctl;
  profile.notify_state_profile = contract.notify_state_profile;
  profile.capabilities = contract.capabilities;
  if (!contract.profile_name.empty())
    profile.name = contract.profile_name;
  if (!contract.operating_system_release.empty())
    profile.operating_system_release = contract.operating_system_release;
  if (contract.operating_system_revision != 0)
    profile.operating_system_revision = contract.operating_system_revision;
  if (!contract.kernel_version.empty())
    profile.version = contract.kernel_version;
  return profile;
}

} // namespace ilemu
