// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "graphics/gles_program_target.hpp"
#include <android/hardware_buffer.h>

namespace ilemu {
class AndroidProgramTarget : public GlesProgramTarget {
public:
    using GlesProgramTarget::GlesProgramTarget;
    [[nodiscard]] Api api() const final { return Api::AndroidHardwareBuffer; }
    [[nodiscard]] virtual AHardwareBuffer* buffer() const = 0;
    // Transfers ownership. -1 denotes an already completed Vulkan submission.
    [[nodiscard]] virtual int take_ready_fd() = 0;
    // Consumes the EGL native fence and publishes the completed shader target.
    [[nodiscard]] virtual bool publish(int ready_fd) = 0;
};
}
