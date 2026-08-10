#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <vector>

#include "ilemu/content_identity.hpp"
#include "ilemu/file_page_cache.hpp"

namespace ilemu {

struct HostFileWatchDrain {
  std::vector<std::filesystem::path> changed_paths;
  std::vector<std::filesystem::path> dirty_subtrees;
  bool overflow{};
};

// A bounded, serialized host filesystem watcher. Notifications are only dirty
// hints: a file is published after two matching descriptor observations with
// a stable generation and content identity. All confirmed changes enter the
// same GuestFileGenerationRegistry used by guest VFS operations.
class HostFileWatcher {
public:
  explicit HostFileWatcher(std::filesystem::path root,
                           std::size_t maximum_pending = 512);
  ~HostFileWatcher();

  HostFileWatcher(const HostFileWatcher &) = delete;
  HostFileWatcher &operator=(const HostFileWatcher &) = delete;

  // Polls the non-blocking notification queue and returns immediately.
  void poll();

  // Publishes only stable paths. Dirty subtrees from an overflow are drained
  // only when requested by an idle/background caller.
  [[nodiscard]] HostFileWatchDrain publish_stable(
      GuestFileGenerationRegistry &registry,
      std::size_t maximum_events = 64,
      bool include_dirty_subtrees = true);

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] std::size_t pending_count() const noexcept;

private:
  struct StableSample {
    GuestFileGeneration generation;
    ContentIdentity content_identity;

    friend constexpr bool operator==(const StableSample &,
                                     const StableSample &) = default;
  };

  struct PendingPath {
    GuestFileMutationKind mutation{GuestFileMutationKind::Write};
    std::chrono::steady_clock::time_point last_event{};
    std::optional<StableSample> sample;
  };

  void add_watch_tree(const std::filesystem::path &directory);
  void add_watch(const std::filesystem::path &directory);
  void remove_watch(int watch_descriptor);
  void queue_path(const std::filesystem::path &path,
                  GuestFileMutationKind mutation);
  void queue_dirty_subtree(const std::filesystem::path &path);
  void handle_event(const void *event, std::size_t available_bytes);

  std::filesystem::path root_;
  std::size_t maximum_pending_{};
  int notification_descriptor_{-1};
  std::map<int, std::filesystem::path> watches_;
  std::map<std::filesystem::path, PendingPath> pending_;
  std::vector<std::filesystem::path> dirty_subtrees_;
  bool overflow_{false};
};

} // namespace ilemu
