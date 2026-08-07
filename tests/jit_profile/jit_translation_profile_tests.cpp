#include "ilemu/jit_translation_profile.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

ilemu::ContentIdentity identity_for(std::string value) {
  return ilemu::sha256(std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(value.data()), value.size()});
}

} // namespace

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    "ilemu-jit-translation-profile-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);

  const auto first_identity = identity_for("first executable content");
  const auto replacement_identity = identity_for("replacement content");
  {
    ilemu::JitTranslationProfileStore store{root};
    const auto first = store.profile_for(first_identity);
    first->record(0x1001U);
    first->record(0x2000U);
    const auto replacement = store.profile_for(replacement_identity);
    replacement->record(0x3000U);
  }

  {
    ilemu::JitTranslationProfileStore store{root};
    const auto first = store.profile_for(first_identity);
    const auto replacement = store.profile_for(replacement_identity);
    if (first->snapshot() != std::vector<std::uint64_t>{0x1001U, 0x2000U} ||
        replacement->snapshot() != std::vector<std::uint64_t>{0x3000U}) {
      std::cerr << "content-keyed profiles did not reload\n";
      return 1;
    }
    if (store.profile_for(ilemu::sha256(std::span<const std::byte>{
            reinterpret_cast<const std::byte *>("other"), 5U}))
            ->snapshot()
            .size() != 0U) {
      std::cerr << "different executable content reused a profile\n";
      return 1;
    }
  }

  std::filesystem::remove_all(root, error);
  return 0;
}
