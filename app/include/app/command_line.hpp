// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

namespace ilemu {

class SessionHost;

// Shared command syntax for desktop and embedded frontends. The caller owns
// the host services and runs one command at a time.
int run_command_line(int argc, char** argv, SessionHost& host);

} // namespace ilemu
