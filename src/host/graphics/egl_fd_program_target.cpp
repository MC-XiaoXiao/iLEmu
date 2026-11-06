/* SPDX-License-Identifier: MPL-2.0 */
#include "egl_fd_program_target.hpp"
#include "graphics/display.hpp"
#include "graphics/host_graphics.hpp"
#include <EGL/egl.h>
#include <GLES2/gl2ext.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
namespace ilemu {
namespace {
    template <class Function> Function proc(const char* name)
    {
        return reinterpret_cast<Function>(eglGetProcAddress(name));
    }
    void check(const char* operation)
    {
        if (glGetError() != GL_NO_ERROR)
            throw std::runtime_error { operation };
    }
    GLuint import_semaphore(int fd)
    {
        GLuint semaphore;
        proc<PFNGLGENSEMAPHORESEXTPROC>("glGenSemaphoresEXT")(1, &semaphore);
        proc<PFNGLIMPORTSEMAPHOREFDEXTPROC>("glImportSemaphoreFdEXT")(
            semaphore, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
        if (glGetError() != GL_NO_ERROR) {
            close(fd);
            proc<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT")(
                1, &semaphore);
            throw std::runtime_error { "import program semaphore into GL" };
        }
        return semaphore;
    }
    void wait_for_image(GLuint texture, int fd)
    {
        const auto semaphore = import_semaphore(fd);
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        proc<PFNGLWAITSEMAPHOREEXTPROC>("glWaitSemaphoreEXT")(
            semaphore, 0, nullptr, 1, &texture, &layout);
        proc<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT")(
            1, &semaphore);
        check("queue external program image wait");
    }
}
std::optional<GlesProgramTarget::DeviceIdentity> EglFdProgramTarget::supported()
{
    const auto* gl = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const auto extensions = std::string { " " } + (gl ? gl : "") + " ";
    for (const auto* name :
        { " GL_EXT_memory_object_fd ", " GL_EXT_semaphore_fd " })
        if (extensions.find(name) == std::string::npos)
            return std::nullopt;
    // GLES2 does not expose internal-format queries. EXT_memory_object
    // texture storage uses optimal tiling, matching the Vulkan allocation.
    GLint count;
    glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &count);
    // This renderer owns one Vulkan physical device, not a device group.
    if (count != 1)
        return std::nullopt;
    GlesProgramTarget::DeviceIdentity device;
    proc<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>("glGetUnsignedBytei_vEXT")(
        GL_DEVICE_UUID_EXT, 0, device.uuid.data());
    proc<PFNGLGETUNSIGNEDBYTEVEXTPROC>("glGetUnsignedBytevEXT")(
        GL_DRIVER_UUID_EXT, device.driver.data());
    check("query program device UUID");
    return device;
}
EglFdProgramTarget::EglFdProgramTarget(
    GLuint texture, std::shared_ptr<GlesProgramTarget> input)
    : texture_ { texture }
    , input_ { std::static_pointer_cast<FdProgramTarget>(std::move(input)) }
{
    proc<PFNGLCREATEMEMORYOBJECTSEXTPROC>("glCreateMemoryObjectsEXT")(
        1, &memory_);
    try {
        const GLint dedicated = GL_TRUE;
        proc<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(
            "glMemoryObjectParameterivEXT")(
            memory_, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        const auto fd = input_->export_memory_fd();
        proc<PFNGLIMPORTMEMORYFDEXTPROC>("glImportMemoryFdEXT")(memory_,
            input_->allocation_size(), GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
        if (glGetError() != GL_NO_ERROR) {
            close(fd);
            throw std::runtime_error { "import program allocation into GL" };
        }
        const auto descriptor = input_->surface()->descriptor();
        glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
        proc<PFNGLTEXSTORAGEMEM2DEXTPROC>("glTexStorageMem2DEXT")(GL_TEXTURE_2D,
            1, GL_RGBA8_OES, static_cast<GLsizei>(descriptor.width),
            static_cast<GLsizei>(descriptor.height), memory_, 0);
        check("bind external program image");
        wait_for_image(texture_, input_->export_ready_fd());
    } catch (...) {
        proc<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT")(
            1, &memory_);
        throw;
    }
}
EglFdProgramTarget::~EglFdProgramTarget()
{
    proc<PFNGLDELETEMEMORYOBJECTSEXTPROC>("glDeleteMemoryObjectsEXT")(
        1, &memory_);
}
bool EglFdProgramTarget::publish(DisplayFrame& frame)
{
    const auto semaphore = import_semaphore(input_->begin_publication());
    const GLenum layout = GL_LAYOUT_GENERAL_EXT;
    proc<PFNGLSIGNALSEMAPHOREEXTPROC>("glSignalSemaphoreEXT")(
        semaphore, 0, nullptr, 1, &texture_, &layout);
    glFlush();
    proc<PFNGLDELETESEMAPHORESEXTPROC>("glDeleteSemaphoresEXT")(1, &semaphore);
    check("publish external program image");
    // Acquire the shared allocation again after Vulkan has consumed it.
    // This also orders subsequent GL writes without a CPU/GPU wait.
    wait_for_image(texture_, input_->publish());
    glFlush();
    frame.pixels.clear();
    frame.host_surface = input_->surface();
    return true;
}
}
