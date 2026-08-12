#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/macho.hpp"
#include "test_support.hpp"

namespace {

using namespace ilemu;
using ilemu::test::find_existing;
using ilemu::test::read_binary_file;
using ilemu::test::require;

void resolver_contract_suite() {
  const std::array responder_candidates{
      std::filesystem::path{"build/rootfs/usr/sbin/mDNSResponder"},
      std::filesystem::path{"rootfs/usr/sbin/mDNSResponder"},
  };
  const auto responder_path = find_existing(responder_candidates);
  require(!responder_path.empty(), "firmware mDNSResponder is unavailable");
  const auto responder = MachOImage::parse(responder_path);
  for (const auto symbol : {"_recvmsg", "_sendto", "_kevent", "_launch_msg",
                            "_launch_data_get_fd", "_udsserver_init"}) {
    require(responder.find_symbol(symbol) != nullptr,
            "firmware mDNSResponder boundary changed");
  }
  const auto responder_bytes = read_binary_file(responder_path);
  for (const auto contract :
       {"iPhone-%02X%02X%02X%02X%02X%02X",
        "Lockdown hasn't set user-specified local hostname",
        "/var/run/mDNSResponder"}) {
    require(responder_bytes.find(contract) != std::string::npos,
            "firmware mDNSResponder resolver contract changed");
  }

  const std::array plist_candidates{
      std::filesystem::path{"build/rootfs/System/Library/LaunchDaemons/"
                            "com.apple.mDNSResponder.plist"},
      std::filesystem::path{"rootfs/System/Library/LaunchDaemons/"
                            "com.apple.mDNSResponder.plist"},
  };
  const auto plist_path = find_existing(plist_candidates);
  require(!plist_path.empty(), "firmware mDNSResponder plist is unavailable");
  const auto plist = read_binary_file(plist_path);
  for (const auto contract :
       {"<string>-launchd</string>", "<key>Listeners</key>",
        "<string>/var/run/mDNSResponder</string>"}) {
    require(plist.find(contract) != std::string::npos,
            "firmware launchd resolver contract changed");
  }

  const std::array library_candidates{
      std::filesystem::path{"build/rootfs/usr/lib/libSystem.dylib"},
      std::filesystem::path{"rootfs/usr/lib/libSystem.dylib"},
  };
  const auto library_path = find_existing(library_candidates);
  require(!library_path.empty(), "firmware libSystem is unavailable");
  const auto library = MachOImage::parse(library_path);
  for (const auto symbol :
       {"_DNSServiceGetAddrInfo", "_DNSServiceProcessResult",
        "_getaddrinfo_async_start", "_getaddrinfo_async_handle_reply"}) {
    require(library.find_symbol(symbol) != nullptr,
            "firmware resolver API boundary changed");
  }
  const auto library_bytes = read_binary_file(library_path);
  require(library_bytes.find("mDNSResponderSystemLibraries-120.0.0.7") !=
                  std::string::npos &&
              library_bytes.find("/var/run/mDNSResponder") != std::string::npos,
          "firmware DNSService v120 transport changed");

  const std::array probe_candidates{
      std::filesystem::path{"guest/dns_async_probe"},
      std::filesystem::path{"build/guest/dns_async_probe"},
  };
  const auto probe_path = find_existing(probe_candidates);
  if (probe_path.empty())
    return;
  const auto probe = MachOImage::parse(probe_path);
  require(mach_cpu_name(probe.cpu_type(), probe.cpu_subtype()) == "ARMv6" &&
              mach_file_type_name(probe.file_type()) == "executable" &&
              probe.entry_point().has_value() &&
              probe.dynamic_linker() ==
                  std::optional<std::string>{"/usr/lib/dyld"} &&
              probe.unknown_commands().empty() && probe.dylibs().size() == 3,
          "guest resolver probe is not an old-dyld ARMv6 executable");
  for (const auto dependency :
       {"/System/Library/Frameworks/SystemConfiguration.framework/"
        "SystemConfiguration",
        "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",
        "/usr/lib/libSystem.B.dylib"}) {
    bool found = false;
    for (const auto &dylib : probe.dylibs())
      found = found || dylib.path == dependency;
    require(found, "guest service probe lost a firmware dependency");
  }
  for (const auto symbol :
       {"_start", "_getaddrinfo_async_start", "_getaddrinfo_async_handle_reply",
        "_mach_msg", "_freeaddrinfo", "_DNSServiceGetAddrInfo",
        "_DNSServiceProcessResult", "_SCDynamicStoreCreate",
        "_SCDynamicStoreSetValue", "_SCDynamicStoreCopyValue",
        "_SCDynamicStoreSetNotificationKeys",
        "_SCDynamicStoreCreateRunLoopSource", "_SCNetworkReachabilityGetFlags",
        "_SCNetworkReachabilityCreateWithName",
        "_SCNetworkReachabilityScheduleWithRunLoop",
        "_CFRunLoopRemoveSource"}) {
    require(probe.find_symbol(symbol) != nullptr,
            "guest resolver probe lost a firmware API boundary");
  }
  const auto probe_bytes = read_binary_file(probe_path);
  for (const auto marker :
       {"PROBE libinfo callback", "PROBE dnssd callback",
        "PROBE dnssd A callback", "PROBE dnssd AAAA callback", "PROBE complete",
        "PROBE sc store callback", "PROBE sc reachability flags",
        "PROBE sc reachability callback", "iPhone-021a543a0002.local"}) {
    require(probe_bytes.find(marker) != std::string::npos,
            "guest resolver probe contract changed");
  }

  const KernelSharedState shared_state;
  const auto ethernet = shared_state.network_interfaces.find("en0");
  const auto expected_mac = virtual_network::interface_mac_address;
  require(ethernet != shared_state.network_interfaces.end() &&
              ethernet->second.link_address_length == expected_mac.size() &&
              ethernet->second.link_address == expected_mac,
          "probe fallback name no longer matches virtual en0");
}

} // namespace

int main() {
  return ilemu::test::run_suite("resolver contract",
                                     resolver_contract_suite);
}
