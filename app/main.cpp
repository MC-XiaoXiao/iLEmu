// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "app/command_line.hpp"
#include "app/desktop_host.hpp"

int main(int argc, char** argv)
{
    ilemu::DesktopHost host;
    return ilemu::run_command_line(argc, argv, host);
}
