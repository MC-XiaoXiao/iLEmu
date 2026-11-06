/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
namespace ilemu {
struct DisplayFrame;
class EglProgramTarget {
public:
    virtual ~EglProgramTarget() = default;
    [[nodiscard]] virtual bool publish(DisplayFrame& frame) = 0;
    [[nodiscard]] virtual bool immutable_storage() const { return false; }
};
}
