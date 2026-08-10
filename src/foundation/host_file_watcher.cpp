#include "ilemu/host_file_watcher.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ilemu {
namespace {

constexpr auto stable_delay = std::chrono::milliseconds{50};
constexpr std::size_t maximum_watches = 16'384;
constexpr std::size_t maximum_dirty_subtrees = 64;

[[nodiscard]] int mutation_priority(GuestFileMutationKind mutation) {
  switch (mutation) {
  case GuestFileMutationKind::InstallReplace:
  case GuestFileMutationKind::Rename:
  case GuestFileMutationKind::Unlink:
    return 3;
  case GuestFileMutationKind::Truncate:
  case GuestFileMutationKind::SharedWriteback:
    return 2;
  case GuestFileMutationKind::Write:
    return 1;
  case GuestFileMutationKind::Observation:
    return 0;
  }
  return 0;
}

[[nodiscard]] std::filesystem::path normalize_path(
    const std::filesystem::path &path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (error) {
    error.clear();
    normalized = std::filesystem::absolute(path, error);
  }
  return (error ? path : normalized).lexically_normal();
}

[[nodiscard]] bool is_in_subtree(const std::filesystem::path &path,
                                 const std::filesystem::path &subtree) {
  if (path == subtree) return true;
  const auto relative = path.lexically_relative(subtree);
  return !relative.empty() && relative != "." &&
         relative.begin() != relative.end() && *relative.begin() != "..";
}

#if defined(__linux__)
[[nodiscard]] GuestFileGeneration generation_from_stat(
    const struct stat &file_stat) {
  return GuestFileGeneration{
      static_cast<std::uint64_t>(file_stat.st_dev),
      static_cast<std::uint64_t>(file_stat.st_ino),
      static_cast<std::uint64_t>(file_stat.st_size),
      static_cast<std::int64_t>(file_stat.st_mtim.tv_sec),
      static_cast<std::int64_t>(file_stat.st_mtim.tv_nsec),
      static_cast<std::int64_t>(file_stat.st_ctim.tv_sec),
      static_cast<std::int64_t>(file_stat.st_ctim.tv_nsec)};
}
#endif

} // namespace

HostFileWatcher::HostFileWatcher(std::filesystem::path root,
                                 std::size_t maximum_pending)
    : root_{normalize_path(root)},
      maximum_pending_{std::max<std::size_t>(maximum_pending, 1U)} {
#if defined(__linux__)
  std::error_code error;
  if (!std::filesystem::is_directory(root_, error) || error) return;
  notification_descriptor_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (notification_descriptor_ < 0) return;
  add_watch(root_);
  // Cover the high-value mutable roots immediately without walking them.
  // Descriptor-generation validation remains the correctness fallback until
  // idle registration reaches their descendants.
  constexpr std::array<std::string_view, 4> mutable_roots{
      "Applications", "private/var", "private/var/mobile/Applications",
      "private/var/staging"};
  for (const auto relative : mutable_roots) {
    const auto directory = root_ / relative;
    std::error_code directory_error;
    if (std::filesystem::is_directory(directory, directory_error) &&
        !directory_error) {
      add_watch(directory);
      queue_watch_tree(directory);
    }
  }
  queue_watch_tree(root_);
#else
  static_cast<void>(root);
#endif
}

HostFileWatcher::~HostFileWatcher() {
#if defined(__linux__)
  if (notification_descriptor_ >= 0) {
    static_cast<void>(::close(notification_descriptor_));
    notification_descriptor_ = -1;
  }
#endif
}

bool HostFileWatcher::enabled() const noexcept {
  return notification_descriptor_ >= 0;
}

std::size_t HostFileWatcher::pending_count() const noexcept {
  return pending_.size();
}

std::size_t HostFileWatcher::watch_count() const noexcept {
  return watches_.size();
}

bool HostFileWatcher::registration_pending() const noexcept {
  return active_registration_.has_value() || !registration_queue_.empty();
}

void HostFileWatcher::queue_watch_tree(
    const std::filesystem::path &directory) {
#if defined(__linux__)
  if (notification_descriptor_ < 0) return;
  const auto normalized = directory.lexically_normal();
  if (active_registration_ &&
      active_registration_->directory == normalized) {
    return;
  }
  if (completed_registrations_.contains(normalized) ||
      !queued_registrations_.insert(normalized).second) {
    return;
  }
  registration_queue_.push_back(normalized);
#else
  static_cast<void>(directory);
#endif
}

void HostFileWatcher::advance_registration(std::size_t maximum_entries) {
#if defined(__linux__)
  if (notification_descriptor_ < 0 || maximum_entries == 0U) return;
  std::size_t inspected{};
  while (inspected < maximum_entries) {
    if (!active_registration_) {
      while (!registration_queue_.empty() && !active_registration_) {
        auto directory = std::move(registration_queue_.front());
        registration_queue_.pop_front();
        queued_registrations_.erase(directory);
        if (completed_registrations_.contains(directory)) continue;
        if (watches_.size() >= maximum_watches) {
          queue_dirty_subtree(directory);
          return;
        }
        add_watch(directory);
        ++inspected;
        std::error_code iterator_error;
        std::filesystem::directory_iterator iterator{
            directory,
            std::filesystem::directory_options::skip_permission_denied,
            iterator_error};
        if (iterator_error) {
          queue_dirty_subtree(directory);
          completed_registrations_.insert(std::move(directory));
          continue;
        }
        active_registration_.emplace(ActiveRegistration{
            std::move(directory), std::move(iterator), {}});
        if (inspected >= maximum_entries) return;
      }
      if (!active_registration_) return;
    }

    auto &registration = *active_registration_;
    if (registration.iterator == registration.end) {
      completed_registrations_.insert(registration.directory);
      active_registration_.reset();
      continue;
    }
    const auto entry = *registration.iterator;
    std::error_code status_error;
    const auto status = entry.symlink_status(status_error);
    if (!status_error && std::filesystem::is_directory(status)) {
      queue_watch_tree(entry.path());
    } else if (status_error) {
      queue_dirty_subtree(registration.directory);
    }
    std::error_code increment_error;
    registration.iterator.increment(increment_error);
    if (increment_error) {
      queue_dirty_subtree(registration.directory);
      completed_registrations_.insert(registration.directory);
      active_registration_.reset();
    }
    ++inspected;
  }
#else
  static_cast<void>(maximum_entries);
#endif
}

void HostFileWatcher::add_watch(const std::filesystem::path &directory) {
#if defined(__linux__)
  const auto normalized = directory.lexically_normal();
  if (watched_directories_.contains(normalized)) return;
  if (notification_descriptor_ < 0 || watches_.size() >= maximum_watches) {
    queue_dirty_subtree(directory);
    return;
  }
  constexpr auto mask = IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_CREATE |
                        IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                        IN_MOVE_SELF;
  const auto watch_descriptor =
      ::inotify_add_watch(notification_descriptor_, normalized.c_str(), mask);
  if (watch_descriptor < 0) {
    queue_dirty_subtree(directory);
    return;
  }
  // The initial recursive iterator is rooted at an already canonical path;
  // canonicalizing every directory here would turn watcher setup into a
  // second tree walk. New directories discovered from inotify are normalized
  // before they reach this function.
  watches_[watch_descriptor] = normalized;
  watched_directories_.insert(normalized);
#else
  static_cast<void>(directory);
#endif
}

void HostFileWatcher::remove_watch(int watch_descriptor) {
#if defined(__linux__)
  if (notification_descriptor_ >= 0) {
    static_cast<void>(::inotify_rm_watch(notification_descriptor_,
                                         watch_descriptor));
  }
#else
  static_cast<void>(watch_descriptor);
#endif
  if (const auto watch = watches_.find(watch_descriptor);
      watch != watches_.end()) {
    watched_directories_.erase(watch->second);
    watches_.erase(watch);
  }
}

void HostFileWatcher::queue_dirty_subtree(const std::filesystem::path &path) {
  if (root_.empty()) return;
  auto normalized = normalize_path(path.empty() ? root_ : path);
  if (!is_in_subtree(normalized, root_)) normalized = root_;
  if (std::find(dirty_subtrees_.begin(), dirty_subtrees_.end(), normalized) !=
      dirty_subtrees_.end()) {
    overflow_ = true;
    return;
  }
  if (dirty_subtrees_.size() >= maximum_dirty_subtrees) {
    dirty_subtrees_.clear();
    dirty_subtrees_.push_back(root_);
  } else {
    dirty_subtrees_.push_back(std::move(normalized));
  }
  overflow_ = true;
}

void HostFileWatcher::queue_path(const std::filesystem::path &path,
                                 GuestFileMutationKind mutation) {
  if (root_.empty()) return;
  const auto normalized = normalize_path(path);
  if (!is_in_subtree(normalized, root_)) return;
  const auto now = std::chrono::steady_clock::now();
  if (const auto existing = pending_.find(normalized);
      existing != pending_.end()) {
    if (mutation_priority(mutation) >
        mutation_priority(existing->second.mutation)) {
      existing->second.mutation = mutation;
    }
    existing->second.last_event = now;
    existing->second.sample.reset();
    return;
  }
  if (pending_.size() >= maximum_pending_) {
    queue_dirty_subtree(normalized.parent_path());
    return;
  }
  pending_.emplace(normalized, PendingPath{mutation, now, std::nullopt});
}

void HostFileWatcher::poll() {
#if defined(__linux__)
  if (notification_descriptor_ < 0) return;
  std::array<std::byte, 64U * 1024U> buffer{};
  for (;;) {
    const auto received = ::read(notification_descriptor_, buffer.data(),
                                 buffer.size());
    if (received < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      queue_dirty_subtree(root_);
      break;
    }
    if (received == 0) break;
    std::size_t offset = 0;
    const auto available = static_cast<std::size_t>(received);
    while (offset + sizeof(inotify_event) <= available) {
      const auto *event = reinterpret_cast<const inotify_event *>(
          buffer.data() + static_cast<std::ptrdiff_t>(offset));
      const auto event_size = sizeof(inotify_event) + event->len;
      if (event_size > available - offset) {
        queue_dirty_subtree(root_);
        break;
      }
      handle_event(event, available - offset);
      offset += event_size;
    }
  }
#endif
}

void HostFileWatcher::handle_event(const void *event_data,
                                   std::size_t available_bytes) {
#if defined(__linux__)
  if (event_data == nullptr || available_bytes < sizeof(inotify_event)) return;
  const auto &event = *static_cast<const inotify_event *>(event_data);
  if (event.mask & IN_Q_OVERFLOW) {
    pending_.clear();
    queue_dirty_subtree(root_);
    return;
  }
  const auto watch = watches_.find(event.wd);
  if (watch == watches_.end()) return;
  const auto watched_directory = watch->second;
  if (event.mask & IN_IGNORED) {
    watched_directories_.erase(watch->second);
    watches_.erase(watch);
    return;
  }

  std::filesystem::path path = watched_directory;
  if (event.len != 0U) {
    const auto length =
        std::min<std::size_t>(event.len, available_bytes - sizeof(inotify_event));
    const auto name_length = ::strnlen(event.name, length);
    if (name_length != 0U) path /= std::string{event.name, name_length};
  }
  path = normalize_path(path);

  const auto is_directory = (event.mask & IN_ISDIR) != 0U;
  if (event.mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
    queue_dirty_subtree(watched_directory.parent_path());
    remove_watch(event.wd);
    if (!is_directory) queue_path(path, GuestFileMutationKind::Unlink);
    return;
  }

  GuestFileMutationKind mutation = GuestFileMutationKind::Write;
  if (event.mask & (IN_DELETE | IN_MOVED_FROM)) {
    mutation = GuestFileMutationKind::Unlink;
  } else if (event.mask & (IN_CREATE | IN_MOVED_TO)) {
    mutation = GuestFileMutationKind::InstallReplace;
  }
  if (is_directory && (event.mask & (IN_CREATE | IN_MOVED_TO))) {
    add_watch(path);
    queue_watch_tree(path);
  }
  if (is_directory && (event.mask & (IN_DELETE | IN_MOVED_FROM))) {
    queue_dirty_subtree(watched_directory);
  }
  queue_path(path, mutation);
#else
  static_cast<void>(event_data);
  static_cast<void>(available_bytes);
#endif
}

HostFileWatchDrain HostFileWatcher::publish_stable(
    GuestFileGenerationRegistry &registry, std::size_t maximum_events,
    bool include_dirty_subtrees) {
  HostFileWatchDrain drain;
  drain.overflow = overflow_;
  if (maximum_events != 0U) {
    const auto now = std::chrono::steady_clock::now();
    for (auto iterator = pending_.begin();
         iterator != pending_.end() &&
         drain.changed_paths.size() < maximum_events;) {
      const auto path = iterator->first;
      auto &pending = iterator->second;
      if (now - pending.last_event < stable_delay) {
        ++iterator;
        continue;
      }

#if defined(__linux__)
      struct stat path_stat {};
      if (::stat(path.c_str(), &path_stat) != 0) {
        static_cast<void>(
            registry.publish(path, GuestFileMutationKind::Unlink));
        drain.changed_paths.push_back(path);
        iterator = pending_.erase(iterator);
        continue;
      }
      const auto observed_generation = generation_from_stat(path_stat);
      const auto current_generation = registry.current(path);
      // Guest VFS writes already publish this exact generation through the
      // shared registry. Inotify also reports those host writes; discard the
      // duplicate before opening and hashing the file again. A changed
      // generation (the normal external-edit case) still takes the stable
      // descriptor path below.
      if (current_generation && current_generation->generation &&
          *current_generation->generation == observed_generation &&
          current_generation->last_mutation !=
              GuestFileMutationKind::Observation) {
        iterator = pending_.erase(iterator);
        continue;
      }
      if (!S_ISREG(path_stat.st_mode)) {
        static_cast<void>(registry.publish(path, pending.mutation));
        drain.changed_paths.push_back(path);
        iterator = pending_.erase(iterator);
        continue;
      }

      const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
      if (descriptor < 0) {
        pending.last_event = now;
        pending.sample.reset();
        ++iterator;
        continue;
      }
      struct stat before {};
      struct stat after {};
      const auto opened = ::fstat(descriptor, &before) == 0;
      const auto identity = opened ? sha256_file(descriptor) : std::nullopt;
      const auto stable = opened && identity && ::fstat(descriptor, &after) == 0 &&
                          generation_from_stat(before) ==
                              generation_from_stat(after);
      if (!stable) {
        static_cast<void>(::close(descriptor));
        pending.last_event = now;
        pending.sample.reset();
        ++iterator;
        continue;
      }
      const StableSample sample{generation_from_stat(after), *identity};
      if (!pending.sample || *pending.sample != sample) {
        pending.sample = sample;
        pending.last_event = now;
        static_cast<void>(::close(descriptor));
        ++iterator;
        continue;
      }
      const auto current = registry.current(path);
      const auto already_published =
          current && current->generation &&
          *current->generation == sample.generation &&
          current->last_mutation != GuestFileMutationKind::Observation;
      if (!already_published) {
        static_cast<void>(
            registry.publish_descriptor(path, descriptor, pending.mutation));
        drain.changed_paths.push_back(path);
      }
      static_cast<void>(::close(descriptor));
#else
      static_cast<void>(registry.publish(path, pending.mutation));
      drain.changed_paths.push_back(path);
#endif
      iterator = pending_.erase(iterator);
    }
  }

  if (include_dirty_subtrees) {
    constexpr std::size_t maximum_returned_dirty_subtrees = 16;
    const auto count = std::min(maximum_returned_dirty_subtrees,
                                dirty_subtrees_.size());
    drain.dirty_subtrees.insert(
        drain.dirty_subtrees.end(), dirty_subtrees_.begin(),
        dirty_subtrees_.begin() + static_cast<std::ptrdiff_t>(count));
    dirty_subtrees_.erase(
        dirty_subtrees_.begin(),
        dirty_subtrees_.begin() + static_cast<std::ptrdiff_t>(count));
    if (dirty_subtrees_.empty()) overflow_ = false;
  }
  drain.overflow = drain.overflow || overflow_;
  return drain;
}

} // namespace ilemu
