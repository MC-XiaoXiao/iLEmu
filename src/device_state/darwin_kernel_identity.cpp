#include "device_state/darwin_kernel_identity.hpp"

namespace ilemu {

DarwinKernelIdentity::DarwinKernelIdentity(
    std::string_view darwin_release, std::string_view ios_build)
    : name { "darwin" + std::string { darwin_release } },
      operating_system_release { darwin_release },
      version { "Darwin Kernel Version " + operating_system_release +
                ": iLemu compatibility kernel; " + name + "/RELEASE_ARM" },
      build_version { ios_build }
{
}

} // namespace ilemu
