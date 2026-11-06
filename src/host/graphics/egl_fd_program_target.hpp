/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "egl_program_target.hpp"
#include "fd_program_target.hpp"
#include <GLES2/gl2.h>
#include <optional>
namespace ilemu {
class EglFdProgramTarget final : public EglProgramTarget {
public:
    [[nodiscard]] static std::optional<GlesProgramTarget::DeviceIdentity>
    supported();
    EglFdProgramTarget(
        GLuint texture, std::shared_ptr<GlesProgramTarget> input);
    ~EglFdProgramTarget() override;
    bool publish(DisplayFrame& frame) override;
    bool immutable_storage() const override { return true; }

private:
    GLuint texture_, memory_ { };
    std::shared_ptr<FdProgramTarget> input_;
};
}
