#pragma once

#include <utility>

#include "runtime/boot_options.hpp"

namespace ilemu {

class Output;
class SessionHost;

// Owns one boot, including guest scheduling, lifecycle, time and shutdown.
// Options and host services are independent of command-line argument syntax.
class EmulatorSession {
public:
    EmulatorSession(BootOptions options, SessionHost& host, Output& output)
        : options_ { std::move(options) }
        , host_ { host }
        , output_ { output }
    {
    }
    EmulatorSession(const EmulatorSession&) = delete;
    EmulatorSession& operator=(const EmulatorSession&) = delete;

    void run();

private:
    BootOptions options_;
    SessionHost& host_;
    Output& output_;
    bool started_ { };
};

} // namespace ilemu
