// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "android_program_target.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>

namespace ilemu {
struct DisplayFrame;
// Owned by the EGL target. GL owns the texture; this retains its imported
// storage and the Vulkan device until the target is replaced or retired.
class EglAndroidProgramTarget final {
public:
    [[nodiscard]] static bool supported(EGLDisplay display);
    EglAndroidProgramTarget(
        EGLDisplay display, std::shared_ptr<GlesProgramTarget> input);
    ~EglAndroidProgramTarget();
    EglAndroidProgramTarget(const EglAndroidProgramTarget&) = delete;
    EglAndroidProgramTarget& operator=(const EglAndroidProgramTarget&) = delete;
    [[nodiscard]] bool publish(DisplayFrame& frame);

private:
    EGLDisplay display_;
    EGLImageKHR image_ { EGL_NO_IMAGE_KHR };
    std::shared_ptr<GlesProgramTarget> input_;
};
}
