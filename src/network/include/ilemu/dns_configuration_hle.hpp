#pragma once

namespace ilemu {

class UserlandHleRegistry;

// Supplies the firmware mDNSResponder with the deterministic virtual DNS
// endpoint without creating resolver files inside the guest root filesystem.
void register_dns_configuration_hle(UserlandHleRegistry& registry);

} // namespace ilemu
