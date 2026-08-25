#pragma once

#include <array>

namespace ilemu {

class GlesMatrix {
public:
    GlesMatrix();
    explicit GlesMatrix(std::array<float, 16> values);

    [[nodiscard]] static GlesMatrix translation(float x, float y, float z);
    [[nodiscard]] static GlesMatrix scale(float x, float y, float z);
    [[nodiscard]] static GlesMatrix rotation(
        float degrees, float x, float y, float z);
    [[nodiscard]] static GlesMatrix orthographic(float left, float right,
        float bottom, float top, float near_value, float far_value);
    [[nodiscard]] static GlesMatrix frustum(float left, float right,
        float bottom, float top, float near_value, float far_value);

    [[nodiscard]] GlesMatrix operator*(const GlesMatrix& right) const;
    [[nodiscard]] std::array<float, 4> transform(
        const std::array<float, 4>& vector) const;
    [[nodiscard]] const std::array<float, 16>& values() const
    {
        return values_;
    }

private:
    std::array<float, 16> values_ { };
};

} // namespace ilemu
