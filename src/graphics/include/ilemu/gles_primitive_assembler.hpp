#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "ilemu/gles_rasterizer.hpp"

namespace ilemu {

// Converts GLES primitive topologies that need host-independent emulation to
// triangles. Keeping this above the render backends gives Vulkan and software
// identical line-width and attribute-interpolation behavior.
class GlesPrimitiveBatch {
public:
    [[nodiscard]] std::span<const GlesRasterVertex> vertices() const;
    [[nodiscard]] std::uint32_t mode() const { return mode_; }
    [[nodiscard]] bool ignores_culling() const { return ignores_culling_; }

private:
    friend class GlesPrimitiveAssembler;

    std::span<const GlesRasterVertex> source_;
    std::optional<std::vector<GlesRasterVertex>> expanded_;
    std::uint32_t mode_ { };
    bool ignores_culling_ { };
};

class GlesPrimitiveAssembler {
public:
    [[nodiscard]] static bool supports(std::uint32_t mode);
    [[nodiscard]] static std::size_t minimum_vertex_count(std::uint32_t mode);
    [[nodiscard]] static std::optional<GlesPrimitiveBatch> assemble(
        std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
        const GlesRasterState& state);
};

} // namespace ilemu
