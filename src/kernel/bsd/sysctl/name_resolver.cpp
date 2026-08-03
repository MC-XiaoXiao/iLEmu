#include "ilemu/darwin_sysctl.hpp"

#include <array>

namespace ilemu::darwin::sysctl {
namespace {

struct NamedObject {
  std::string_view name;
  std::uint32_t control;
  std::uint32_t selector;
};

// These selectors are fixed by xnu-792 bsd/sys/sysctl.h. Keep this table
// limited to nodes for which the compatibility kernel has a value handler.
constexpr std::array named_objects{
    NamedObject{"kern.ostype", control_kernel, kernel_operating_system_type},
    NamedObject{"kern.osrelease", control_kernel,
                kernel_operating_system_release},
    NamedObject{"kern.osrevision", control_kernel,
                kernel_operating_system_revision},
    NamedObject{"kern.version", control_kernel, kernel_version},
    NamedObject{"kern.maxvnodes", control_kernel, 5},
    NamedObject{"kern.maxproc", control_kernel, 6},
    NamedObject{"kern.maxfiles", control_kernel, 7},
    NamedObject{"kern.argmax", control_kernel, 8},
    NamedObject{"kern.hostname", control_kernel, 10},
    NamedObject{"kern.netboot", control_kernel, 40},
    NamedObject{"kern.osversion", control_kernel, kernel_build_version},
    NamedObject{"hw.machine", control_hardware, hardware_machine},
    NamedObject{"hw.model", control_hardware, hardware_model},
    NamedObject{"hw.ncpu", control_hardware, 3},
    NamedObject{"hw.byteorder", control_hardware, 4},
    NamedObject{"hw.physmem", control_hardware, 5},
    NamedObject{"hw.usermem", control_hardware, 6},
    NamedObject{"hw.pagesize", control_hardware, 7},
    NamedObject{"hw.floatingpoint", control_hardware, 11},
    NamedObject{"hw.vectorunit", control_hardware, 13},
    NamedObject{"hw.memsize", control_hardware, 24},
    NamedObject{"hw.availcpu", control_hardware, 25},
};

} // namespace

std::optional<ObjectIdentifier> resolve_name(std::string_view name) {
  if (name.ends_with('.')) {
    name.remove_suffix(1);
  }
  for (const auto &object : named_objects) {
    if (object.name == name) {
      return ObjectIdentifier{{object.control, object.selector}, 2};
    }
  }
  return std::nullopt;
}

} // namespace ilemu::darwin::sysctl
