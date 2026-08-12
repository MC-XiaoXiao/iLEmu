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

// Adapts host-backed device state to Darwin notify without replacing notifyd.
// Registration, token allocation, Mach delivery, and cancellation stay native;
// only the state read for explicitly provided device keys is supplied here.
class DarwinNotifyStateHle {
public:
  using StateProvider = std::function<std::uint64_t()>;
  using NotificationDispatcher = std::function<void(
      std::uint32_t process_id, std::uint32_t port_name,
      std::uint32_t token)>;

  explicit DarwinNotifyStateHle(UserlandHleRegistry &registry);
  ~DarwinNotifyStateHle();

  void set_provider(std::string name, StateProvider provider);
  void set_notification_dispatcher(NotificationDispatcher dispatcher);
  void inherit_state(const DarwinNotifyStateHle &parent);
  void publish(std::string_view name);
  void reset();

private:
  void register_mach_port(UserlandHleCall &call);
  void register_check(UserlandHleCall &call);
  void get_state(UserlandHleCall &call);
  void cancel(UserlandHleCall &call);
  void record_registration(std::string name, std::uint32_t token,
                           std::uint32_t process_id,
                           std::uint32_t port_name);

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
  NotificationDispatcher dispatcher_;
  std::shared_ptr<SharedBus> bus_{std::make_shared<SharedBus>()};
  std::set<RegistrationKey> owned_registrations_;
};

} // namespace ilemu
