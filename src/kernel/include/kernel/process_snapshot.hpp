#pragma once

#include <cstdint>
#include <string>

namespace ilemu {

// A copied observation of the guest process table. Clients never receive a
// mutable process record or hold a kernel lock while formatting diagnostics.
struct ProcessSnapshot {
    std::uint32_t pid { };
    std::uint32_t parent_pid { };
    std::string command;
    std::string executable_path;
    bool exited { };
    bool pid_suspended { };
    bool signal_stopped { };
    std::uint32_t exit_status { };
    std::uint32_t termination_signal { };
};

} // namespace ilemu
