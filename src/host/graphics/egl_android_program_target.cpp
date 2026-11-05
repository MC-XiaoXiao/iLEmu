// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "egl_android_program_target.hpp"
#include "graphics/display.hpp"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace ilemu {
namespace {
    template <typename Function> Function proc(const char* name)
    {
        return reinterpret_cast<Function>(eglGetProcAddress(name));
    }
    void check(EGLBoolean succeeded, const char* operation)
    {
        if (!succeeded)
            throw std::runtime_error { operation };
    }
}

bool EglAndroidProgramTarget::supported(EGLDisplay display)
{
    const auto* egl = eglQueryString(display, EGL_EXTENSIONS);
    const auto* gl = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const auto egl_extensions = std::string { " " } + (egl ? egl : "") + " ";
    const auto gl_extensions = std::string { " " } + (gl ? gl : "") + " ";
    for (const auto* extension : { " EGL_ANDROID_image_native_buffer ",
             " EGL_ANDROID_get_native_client_buffer ",
             " EGL_ANDROID_native_fence_sync ", " EGL_KHR_image_base ",
             " EGL_KHR_wait_sync " })
        if (egl_extensions.find(extension) == std::string::npos)
            return false;
    return gl_extensions.find(" GL_OES_EGL_image ") != std::string::npos;
}

class EglAndroidProgramTarget::Image final {
public:
    Image(EGLDisplay display, std::shared_ptr<void> display_owner,
        AHardwareBuffer* buffer)
        : display_ { display }
        , display_owner_ { std::move(display_owner) }
    {
        const auto client_buffer = proc<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            "eglGetNativeClientBufferANDROID");
        const auto create_image =
            proc<PFNEGLCREATEIMAGEKHRPROC>("eglCreateImageKHR");
        const EGLint attributes[] { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        image_ = create_image(display_, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
            client_buffer(buffer), attributes);
        if (image_ == EGL_NO_IMAGE_KHR)
            throw std::runtime_error { "import shader input into EGL" };
    }
    ~Image()
    {
        proc<PFNEGLDESTROYIMAGEKHRPROC>("eglDestroyImageKHR")(display_, image_);
    }
    [[nodiscard]] EGLImageKHR get() const { return image_; }

private:
    EGLDisplay display_;
    std::shared_ptr<void> display_owner_;
    EGLImageKHR image_;
};

EglAndroidProgramTarget::EglAndroidProgramTarget(EGLDisplay display,
    std::shared_ptr<void> display_owner,
    std::shared_ptr<GlesProgramTarget> input)
    : display_ { display }
    , input_ { std::move(input) }
{
    auto& native = static_cast<AndroidProgramTarget&>(*input_);
    image_ = std::make_shared<Image>(
        display_, std::move(display_owner), native.buffer());
    const auto fd = native.take_ready_fd();
    if (fd >= 0) {
        const EGLint sync_attributes[] { EGL_SYNC_NATIVE_FENCE_FD_ANDROID,
            fd, EGL_NONE };
        const auto create_sync =
            proc<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR");
        const auto sync = create_sync(
            display_, EGL_SYNC_NATIVE_FENCE_ANDROID, sync_attributes);
        if (sync == EGL_NO_SYNC_KHR) {
            close(fd);
            throw std::runtime_error { "import shader input fence" };
        }
        const auto wait_sync =
            proc<PFNEGLWAITSYNCKHRPROC>("eglWaitSyncKHR");
        const auto waited = wait_sync(display_, sync, 0);
        proc<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR")(display_, sync);
        check(waited, "queue shader input GPU wait");
    }
    proc<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        "glEGLImageTargetTexture2DOES")(GL_TEXTURE_2D, image_->get());
    if (glGetError() != GL_NO_ERROR)
        throw std::runtime_error { "bind shader input image" };
    native.retain_import(image_);
}

EglAndroidProgramTarget::~EglAndroidProgramTarget() = default;

bool EglAndroidProgramTarget::publish(DisplayFrame& frame)
{
    const auto create_sync = proc<PFNEGLCREATESYNCKHRPROC>("eglCreateSyncKHR");
    const auto sync =
        create_sync(display_, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR)
        return false;
    glFlush();
    const auto fd = proc<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(
        "eglDupNativeFenceFDANDROID")(display_, sync);
    proc<PFNEGLDESTROYSYNCKHRPROC>("eglDestroySyncKHR")(display_, sync);
    if (fd < 0)
        return false;
    auto& target = static_cast<AndroidProgramTarget&>(*input_);
    if (!target.publish(fd))
        return false;
    frame.pixels.clear();
    frame.host_surface = target.surface();
    return true;
}
}
