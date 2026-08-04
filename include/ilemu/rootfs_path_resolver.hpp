#pragma once

#include <filesystem>
#include <string_view>
#include <utility>

namespace ilemu {

class RootfsPathResolver {
public:
  explicit RootfsPathResolver(std::filesystem::path rootfs)
      : rootfs_{std::move(rootfs)} {}

  [[nodiscard]] std::filesystem::path
  resolve(std::string_view guest_path,
          const std::filesystem::path &guest_working_directory = "/",
          bool follow_final_symlink = true) const;

private:
  std::filesystem::path rootfs_;
};

} // namespace ilemu
