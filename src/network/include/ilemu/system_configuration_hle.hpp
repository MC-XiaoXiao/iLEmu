#pragma once

namespace ilemu {

class UserlandHleCall;
class UserlandHleRegistry;

// Keeps the firmware's SystemConfiguration interface enumeration and IOKit
// object construction intact while avoiding an unbounded readiness wait when
// the legacy InterfaceNamer dynamic-store marker is absent.
class SystemConfigurationHle {
public:
  explicit SystemConfigurationHle(UserlandHleRegistry &registry);

private:
  void begin_interface_discovery(UserlandHleCall &call);
};

} // namespace ilemu
