// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

namespace ilemu {
class UserlandHleRegistry;

// Implement the decoder transport used by VideoToolbox's native H264H4 plug-in.
void register_vxd390_video_hle(UserlandHleRegistry& registry);
} // namespace ilemu
