#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace ilemu {

class UserlandHleCall;
class UserlandHleRegistry;

// Some libnotify generations contact notifyd from image initializers, including
// notifyd's own image initialization. Their bootstrap-aware profile supplies
// process-local check tokens while the server is absent, then returns to the
// firmware service once notifyd has checked in.
enum class DarwinNotifyStateProfile : std::uint8_t {
  NativeServerTokens,
  BootstrapAwareServerTokens,
};

// Adapts host-backed device state to Darwin notify without replacing notifyd.
class DarwinNotifyStateHle {
public:
  using StateProvider = std::function<std::uint64_t()>;
  using NotificationDispatcher = std::function<void(
      std::uint32_t process_id, std::uint32_t port_name,
      std::uint32_t token)>;
  using NativeServerReadyQuery = std::function<bool()>;

  explicit DarwinNotifyStateHle(UserlandHleRegistry &registry);
  ~DarwinNotifyStateHle();

  void set_profile(DarwinNotifyStateProfile profile);
  void set_native_server_ready_query(NativeServerReadyQuery query);
  void set_provider(std::string name, StateProvider provider);
  void set_notification_dispatcher(NotificationDispatcher dispatcher);
  void inherit_state(const DarwinNotifyStateHle &parent);
  void publish(std::string_view name);
  void reset();

private:
  void register_mach_port(UserlandHleCall &call);
  void register_check(UserlandHleCall &call);
  void check(UserlandHleCall &call);
  void get_state(UserlandHleCall &call);
  void cancel(UserlandHleCall &call);
  void record_registration(std::string name, std::uint32_t token,
                           std::uint32_t process_id,
                           std::uint32_t port_name);
  [[nodiscard]] std::uint32_t allocate_virtual_token();
  [[nodiscard]] bool native_server_ready() const;

  using RegistrationKey = std::pair<std::uint32_t, std::uint32_t>;
  struct PublishedRegistration {
    std::string name;
    std::function<void()> notify;
  };
  struct SharedBus {
    std::mutex mutex;
    std::map<RegistrationKey, PublishedRegistration> registrations;
  };

  mutable std::mutex mutex_;
  std::map<std::string, StateProvider, std::less<>> providers_;
  std::map<std::uint32_t, std::string> token_names_;
  std::set<std::uint32_t> virtual_tokens_;
  std::uint32_t next_virtual_token_{0x4000'0000U};
  DarwinNotifyStateProfile profile_{
      DarwinNotifyStateProfile::NativeServerTokens};
  NativeServerReadyQuery native_server_ready_query_;
  NotificationDispatcher dispatcher_;
  std::shared_ptr<SharedBus> bus_{std::make_shared<SharedBus>()};
  std::set<RegistrationKey> owned_registrations_;
};

} // namespace ilemu
