// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Adapt guest MBX textured-quad operations to shared-surface
// rendering.

#include "kernel/mbx2d_hle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "foundation/address_space.hpp"
#include "graphics/display.hpp"
#include "graphics/gles_renderer.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "graphics/mbx2d_abi.hpp"
#include "foundation/performance.hpp"
#include "foundation/userland_hle.hpp"

namespace ilemu {
namespace {

    using Point = HostPoint;

    constexpr float edge_tolerance = 0.01F;
    constexpr std::uint64_t maximum_quad_pixels = 16U * 1024U * 1024U;

    bool nearly_equal(float left, float right)
    {
        return std::abs(left - right) <= edge_tolerance;
    }

    bool triangles_share_only_diagonal(const std::array<Point, 4>& quad)
    {
        const auto side = [&](std::size_t vertex) {
            const auto diagonal_x = quad[2].x - quad[0].x;
            const auto diagonal_y = quad[2].y - quad[0].y;
            const auto point_x = quad[vertex].x - quad[0].x;
            const auto point_y = quad[vertex].y - quad[0].y;
            return diagonal_x * point_y - diagonal_y * point_x;
        };
        const auto first = side(1);
        const auto second = side(3);
        return (first > edge_tolerance && second < -edge_tolerance) ||
               (first < -edge_tolerance && second > edge_tolerance);
    }

    std::optional<std::array<Point, 4>> read_quad(
        AddressSpace& memory, std::uint32_t address)
    {
        if (address == 0 ||
            address > std::numeric_limits<std::uint32_t>::max() -
                          7U * sizeof(std::uint32_t)) {
            return std::nullopt;
        }
        std::array<Point, 4> result { };
        for (std::size_t vertex = 0; vertex < result.size(); ++vertex) {
            const auto x = memory.read32(
                address + static_cast<std::uint32_t>(vertex * 8U));
            const auto y = memory.read32(
                address + static_cast<std::uint32_t>(vertex * 8U + 4U));
            if (!x || !y)
                return std::nullopt;
            result[vertex] = { std::bit_cast<float>(*x),
                std::bit_cast<float>(*y) };
            if (!std::isfinite(result[vertex].x) ||
                !std::isfinite(result[vertex].y)) {
                return std::nullopt;
            }
        }
        return result;
    }

    std::optional<std::array<float, 4>> read_perspective(
        AddressSpace& memory, std::uint32_t address)
    {
        if (address == 0 ||
            address > std::numeric_limits<std::uint32_t>::max() -
                          3U * sizeof(std::uint32_t)) {
            return std::nullopt;
        }
        std::array<float, 4> result { };
        for (std::size_t index = 0; index < result.size(); ++index) {
            const auto value =
                memory.read32(address + static_cast<std::uint32_t>(
                                            index * sizeof(std::uint32_t)));
            if (!value)
                return std::nullopt;
            result[index] = std::bit_cast<float>(*value);
            if (!std::isfinite(result[index]) ||
                std::abs(result[index]) <= 1.0e-6F)
                return std::nullopt;
        }
        return result;
    }

    struct TriangleSample {
        std::array<std::size_t, 3> vertices { };
        std::array<float, 3> weights { };
    };

    std::optional<TriangleSample> sample_triangle(
        const std::array<Point, 4>& quad, std::array<std::size_t, 3> vertices,
        Point point)
    {
        const auto& first = quad[vertices[0]];
        const auto& second = quad[vertices[1]];
        const auto& third = quad[vertices[2]];
        const auto denominator = (second.y - third.y) * (first.x - third.x) +
                                 (third.x - second.x) * (first.y - third.y);
        if (std::abs(denominator) <= edge_tolerance)
            return std::nullopt;
        const auto first_weight =
            ((second.y - third.y) * (point.x - third.x) +
                (third.x - second.x) * (point.y - third.y)) /
            denominator;
        const auto second_weight =
            ((third.y - first.y) * (point.x - third.x) +
                (first.x - third.x) * (point.y - third.y)) /
            denominator;
        const auto third_weight = 1.0F - first_weight - second_weight;
        if (first_weight < -edge_tolerance || second_weight < -edge_tolerance ||
            third_weight < -edge_tolerance)
            return std::nullopt;
        return TriangleSample { vertices,
            { first_weight, second_weight, third_weight } };
    }

    std::optional<TriangleSample> sample_quad(
        const std::array<Point, 4>& quad, Point point)
    {
        if (const auto first = sample_triangle(quad, { 0, 1, 2 }, point))
            return first;
        return sample_triangle(quad, { 0, 2, 3 }, point);
    }

    std::optional<Point> interpolate_perspective(
        const std::array<Point, 4>& values,
        const std::array<float, 4>& perspective, const TriangleSample& sample)
    {
        Point numerator { };
        float denominator = 0.0F;
        for (std::size_t index = 0; index < sample.vertices.size(); ++index) {
            const auto vertex = sample.vertices[index];
            const auto weight = perspective[vertex];
            if (!std::isfinite(weight) || std::abs(weight) <= 1.0e-6F)
                return std::nullopt;
            const auto reciprocal = 1.0F / weight;
            const auto coefficient = sample.weights[index] * reciprocal;
            numerator.x += coefficient * values[vertex].x;
            numerator.y += coefficient * values[vertex].y;
            denominator += coefficient;
        }
        if (!std::isfinite(denominator) || std::abs(denominator) <= 1.0e-6F)
            return std::nullopt;
        return Point { numerator.x / denominator, numerator.y / denominator };
    }

} // namespace

void Mbx2dHle::quad_color(UserlandHleCall& call)
{
    const auto destination = resolve(state_.destination);
    const auto positions = read_quad(call.memory(), call.argument(0));
    if (!destination || !positions) {
        call.set_return(mbx2d_abi::failure);
        return;
    }

    execute_quad_color(call, *destination, *positions);
}

bool Mbx2dHle::quad_covers(const QuadCoverage& coverage, HostPoint point)
{
    const auto sample = sample_quad(coverage.positions, point);
    return sample && (!coverage.perspective ||
        interpolate_perspective(coverage.positions, *coverage.perspective,
            *sample).has_value());
}

void Mbx2dHle::execute_quad_color(UserlandHleCall& call,
    const ResolvedSurface& destination_surface,
    const std::array<HostPoint, 4>& position_values,
    std::shared_ptr<CpuDraw> cpu, bool native_attempted)
try {
    const auto* destination = &destination_surface;
    const auto* positions = &position_values;
    auto left_value = (*positions)[0].x;
    auto right_value = left_value;
    auto top_value = (*positions)[0].y;
    auto bottom_value = top_value;
    for (const auto& position : *positions) {
        left_value = std::min(left_value, position.x);
        right_value = std::max(right_value, position.x);
        top_value = std::min(top_value, position.y);
        bottom_value = std::max(bottom_value, position.y);
    }
    if (right_value - left_value <= edge_tolerance ||
        bottom_value - top_value <= edge_tolerance) {
        call.set_return(mbx2d_abi::success);
        return;
    }

    auto left = std::max<std::int64_t>(
        static_cast<std::int64_t>(std::floor(left_value)), 0);
    auto right = std::min<std::int64_t>(
        static_cast<std::int64_t>(std::ceil(right_value)), destination->width);
    auto top = std::max<std::int64_t>(
        static_cast<std::int64_t>(std::floor(top_value)), 0);
    auto bottom = std::min<std::int64_t>(
        static_cast<std::int64_t>(std::ceil(bottom_value)),
        destination->height);
    if (state_.scissor.enabled) {
        left = std::max<std::int64_t>(left, state_.scissor.left);
        top = std::max<std::int64_t>(top, state_.scissor.top);
        right = std::min<std::int64_t>(right, state_.scissor.right);
        bottom = std::min<std::int64_t>(bottom, state_.scissor.bottom);
    }
    if (right <= left || bottom <= top) {
        call.set_return(mbx2d_abi::success);
        return;
    }

    const auto width = right - left;
    const auto height = bottom - top;
    if (static_cast<std::uint64_t>(width) >
        maximum_quad_pixels / static_cast<std::uint64_t>(height)) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    const auto axis_aligned = nearly_equal((*positions)[0].x, left_value) &&
                              nearly_equal((*positions)[0].y, top_value) &&
                              nearly_equal((*positions)[1].x, right_value) &&
                              nearly_equal((*positions)[1].y, top_value) &&
                              nearly_equal((*positions)[2].x, right_value) &&
                              nearly_equal((*positions)[2].y, bottom_value) &&
                              nearly_equal((*positions)[3].x, left_value) &&
                              nearly_equal((*positions)[3].y, bottom_value);
    const auto composite_mode = host_composite_mode(state_);
    auto fallback = [this, destination = *destination,
                        positions = *positions](UserlandHleCall& resumed) {
        execute_quad_color(resumed, destination, positions, { }, true);
    };
    if (!cpu && !native_attempted && axis_aligned &&
        host_graphics_->accelerated() &&
        destination->host_surface && destination->backing &&
        destination->backing->pixel_format == surface_pixel_format_bgra &&
        composite_mode && left <= std::numeric_limits<std::int32_t>::max() &&
        top <= std::numeric_limits<std::int32_t>::max() &&
        static_cast<std::uint64_t>(width) <=
            std::numeric_limits<std::uint32_t>::max() &&
        static_cast<std::uint64_t>(height) <=
            std::numeric_limits<std::uint32_t>::max()) {
        fill_native(call, false, *destination,
            { static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height) },
            call.argument(1), *composite_mode, state_.blend.global_alpha,
            std::move(fallback));
        return;
    }
    if (!cpu && !native_attempted && !axis_aligned &&
        triangles_share_only_diagonal(*positions) &&
        host_graphics_->accelerated() && destination->host_surface &&
        destination->backing &&
        destination->backing->pixel_format == surface_pixel_format_bgra &&
        composite_mode && left <= std::numeric_limits<std::int32_t>::max() &&
        top <= std::numeric_limits<std::int32_t>::max() &&
        static_cast<std::uint64_t>(width) <=
            std::numeric_limits<std::uint32_t>::max() &&
        static_cast<std::uint64_t>(height) <=
            std::numeric_limits<std::uint32_t>::max()) {
        fill_quad_native(call, *destination, *positions,
            { static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height) },
            call.argument(1), *composite_mode, state_.blend.global_alpha,
            std::move(fallback));
        return;
    }
    if (!cpu && (destination->framebuffer ||
        (destination->host_surface && destination->core_surface_id))) {
        prepare_cpu_draw(call, false, { }, *destination,
            static_cast<std::size_t>(width * height) * sizeof(std::uint32_t),
            [this, positions = *positions](UserlandHleCall& resumed,
                const std::optional<ResolvedSurface>&,
                const ResolvedSurface& ready_destination,
                std::shared_ptr<CpuDraw> ready) {
                execute_quad_color(resumed, ready_destination, positions,
                    std::move(ready), true);
            });
        return;
    }
    if (cpu) {
        write_cpu_draw(call, std::move(cpu),
            { static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height) },
            std::vector<std::uint32_t>(
                static_cast<std::size_t>(width * height), call.argument(1)),
            axis_aligned ? std::nullopt
                         : std::optional { QuadCoverage { *positions, { } } });
        return;
    }
    const auto destination_pixels =
        read_region(*destination, left, top, width, height, call);
    if (!destination_pixels) {
        call.set_return(mbx2d_abi::failure);
        return;
    }

    std::vector<std::uint32_t> source_pixels(
        static_cast<std::size_t>(width * height), call.argument(1));
    std::vector<bool> covered;
    if (!axis_aligned) {
        covered.assign(source_pixels.size(), false);
        for (std::int64_t y = 0; y < height; ++y) {
            for (std::int64_t x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(y * width + x);
                covered[index] = sample_quad(
                    *positions, Point { static_cast<float>(left + x) + 0.5F,
                                    static_cast<float>(top + y) + 0.5F })
                                     .has_value();
            }
        }
    }

    auto pixels = composite(
        state_, *destination, left, top, width, height, source_pixels, call);
    if (!pixels) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    if (!axis_aligned) {
        for (std::size_t index = 0; index < covered.size(); ++index) {
            if (!covered[index])
                (*pixels)[index] = (*destination_pixels)[index];
        }
    }
    call.set_return(
        write_region(*destination, left, top, width, height, *pixels, call)
            ? mbx2d_abi::success
            : mbx2d_abi::failure);
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

void Mbx2dHle::quad_copy(
    UserlandHleCall& call, bool context_api, bool perspective_api)
{
    auto* selected_state = select_state(call, context_api);
    if (!selected_state) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    const auto& state = *selected_state;
    const auto first_argument = context_api ? 1U : 0U;
    const auto source = resolve_source(call, state.source);
    const auto destination = resolve(state.destination);
    const auto positions =
        read_quad(call.memory(), call.argument(first_argument));
    auto texture = read_quad(call.memory(), call.argument(first_argument + 1U));
    std::array<float, 4> perspective { 1.0F, 1.0F, 1.0F, 1.0F };
    if (perspective_api) {
        const auto values =
            read_perspective(call.memory(), call.argument(first_argument + 2U));
        if (!values) {
            call.set_return(mbx2d_abi::failure);
            return;
        }
        perspective = *values;
    }
    if (!source || !destination || !positions || !texture) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    if (!source_surface_allowed(*source)) {
        call.set_return(mbx2d_abi::success);
        return;
    }

    execute_quad_copy(call, context_api, *source, *destination,
        *positions, *texture, perspective);
}

void Mbx2dHle::execute_quad_copy(UserlandHleCall& call, bool context_api,
    const ResolvedSurface& source_surface,
    const ResolvedSurface& destination_surface,
    const std::array<HostPoint, 4>& position_values,
    const std::array<HostPoint, 4>& texture_values,
    const std::array<float, 4>& perspective, bool source_imported,
    std::shared_ptr<CpuDraw> cpu, QuadStage stage)
try {
    const auto* selected_state = select_state(call, context_api);
    if (!selected_state) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    const auto& state = *selected_state;
    const auto* source = &source_surface;
    const auto* destination = &destination_surface;
    const auto* positions = &position_values;
    const auto* texture = &texture_values;
    auto left_value = (*positions)[0].x;
    auto right_value = left_value;
    auto top_value = (*positions)[0].y;
    auto bottom_value = top_value;
    for (const auto& position : *positions) {
        left_value = std::min(left_value, position.x);
        right_value = std::max(right_value, position.x);
        top_value = std::min(top_value, position.y);
        bottom_value = std::max(bottom_value, position.y);
    }
    if (right_value - left_value <= edge_tolerance ||
        bottom_value - top_value <= edge_tolerance) {
        call.set_return(mbx2d_abi::success);
        return;
    }

    const auto unclipped_left =
        static_cast<std::int64_t>(std::floor(left_value));
    const auto unclipped_right =
        static_cast<std::int64_t>(std::ceil(right_value));
    const auto unclipped_top = static_cast<std::int64_t>(std::floor(top_value));
    const auto unclipped_bottom =
        static_cast<std::int64_t>(std::ceil(bottom_value));
    auto left = unclipped_left;
    auto right = unclipped_right;
    auto top = unclipped_top;
    auto bottom = unclipped_bottom;
    left = std::max<std::int64_t>(left, 0);
    top = std::max<std::int64_t>(top, 0);
    right = std::min<std::int64_t>(right, destination->width);
    bottom = std::min<std::int64_t>(bottom, destination->height);
    if (state.scissor.enabled) {
        left = std::max<std::int64_t>(left, state.scissor.left);
        top = std::max<std::int64_t>(top, state.scissor.top);
        right = std::min<std::int64_t>(right, state.scissor.right);
        bottom = std::min<std::int64_t>(bottom, state.scissor.bottom);
    }
    if (right <= left || bottom <= top) {
        call.set_return(mbx2d_abi::success);
        return;
    }

    float minimum_u = (*texture)[0].x;
    float maximum_u = minimum_u;
    float minimum_v = (*texture)[0].y;
    float maximum_v = minimum_v;
    for (const auto& coordinate : *texture) {
        minimum_u = std::min(minimum_u, coordinate.x);
        maximum_u = std::max(maximum_u, coordinate.x);
        minimum_v = std::min(minimum_v, coordinate.y);
        maximum_v = std::max(maximum_v, coordinate.y);
    }
    auto source_left = static_cast<std::int64_t>(std::floor(minimum_u));
    auto source_right = static_cast<std::int64_t>(std::ceil(maximum_u));
    auto source_top = static_cast<std::int64_t>(std::floor(minimum_v));
    auto source_bottom = static_cast<std::int64_t>(std::ceil(maximum_v));
    if (source_right <= source_left)
        ++source_right;
    if (source_bottom <= source_top)
        ++source_bottom;
    source_left = std::clamp<std::int64_t>(source_left, 0, source->width);
    source_right = std::clamp<std::int64_t>(source_right, 0, source->width);
    source_top = std::clamp<std::int64_t>(source_top, 0, source->height);
    source_bottom = std::clamp<std::int64_t>(source_bottom, 0, source->height);
    if (source_right <= source_left || source_bottom <= source_top) {
        call.set_return(mbx2d_abi::success);
        return;
    }

    const auto source_width = source_right - source_left;
    const auto source_height = source_bottom - source_top;
    const auto width = right - left;
    const auto height = bottom - top;
    if (static_cast<std::uint64_t>(width) >
        maximum_quad_pixels / static_cast<std::uint64_t>(height)) {
        call.set_return(mbx2d_abi::failure);
        return;
    }

    const auto perspective_affine =
        std::all_of(perspective.begin(), perspective.end(),
            [](float value) { return nearly_equal(value, 1.0F); });
    const auto axis_aligned_affine =
        perspective_affine && nearly_equal((*positions)[0].x, left_value) &&
        nearly_equal((*positions)[0].y, top_value) &&
        nearly_equal((*positions)[1].x, right_value) &&
        nearly_equal((*positions)[1].y, top_value) &&
        nearly_equal((*positions)[2].x, right_value) &&
        nearly_equal((*positions)[2].y, bottom_value) &&
        nearly_equal((*positions)[3].x, left_value) &&
        nearly_equal((*positions)[3].y, bottom_value) &&
        nearly_equal((*texture)[0].x + (*texture)[2].x,
            (*texture)[1].x + (*texture)[3].x) &&
        nearly_equal((*texture)[0].y + (*texture)[2].y,
            (*texture)[1].y + (*texture)[3].y);
    const auto matches_texture = [&](std::array<Point, 4> expected) {
        for (std::size_t index = 0; index < expected.size(); ++index) {
            if (!nearly_equal((*texture)[index].x, expected[index].x) ||
                !nearly_equal((*texture)[index].y, expected[index].y)) {
                return false;
            }
        }
        return true;
    };
    const auto source_left_f = static_cast<float>(source_left);
    const auto source_right_f = static_cast<float>(source_right);
    const auto source_top_f = static_cast<float>(source_top);
    const auto source_bottom_f = static_cast<float>(source_bottom);
    std::optional<HostRotation> host_rotation;
    if (matches_texture({ { { source_left_f, source_top_f },
            { source_right_f, source_top_f },
            { source_right_f, source_bottom_f },
            { source_left_f, source_bottom_f } } })) {
        host_rotation = HostRotation::Identity;
    } else if (matches_texture({ { { source_left_f, source_bottom_f },
                   { source_left_f, source_top_f },
                   { source_right_f, source_top_f },
                   { source_right_f, source_bottom_f } } })) {
        host_rotation = HostRotation::Clockwise90;
    } else if (matches_texture({ { { source_right_f, source_bottom_f },
                   { source_left_f, source_bottom_f },
                   { source_left_f, source_top_f },
                   { source_right_f, source_top_f } } })) {
        host_rotation = HostRotation::Rotate180;
    } else if (matches_texture({ { { source_right_f, source_top_f },
                   { source_right_f, source_bottom_f },
                   { source_left_f, source_bottom_f },
                   { source_left_f, source_top_f } } })) {
        host_rotation = HostRotation::Clockwise270;
    }
    const auto destination_unclipped =
        left == unclipped_left && right == unclipped_right &&
        top == unclipped_top && bottom == unclipped_bottom;
    const auto destination_integral =
        nearly_equal(left_value, static_cast<float>(unclipped_left)) &&
        nearly_equal(right_value, static_cast<float>(unclipped_right)) &&
        nearly_equal(top_value, static_cast<float>(unclipped_top)) &&
        nearly_equal(bottom_value, static_cast<float>(unclipped_bottom));
    const auto composite_mode = host_composite_mode(state);
    if (!source_imported) {
        const auto imported = synchronize_host_source(call, *source);
        if (imported == SurfaceStore::CpuImportResult::PendingGpu) {
            import_source(call, context_api, *source, *destination,
                [this, context_api, position_values, texture_values, perspective,
                    cpu, stage](
                    UserlandHleCall& resumed, const ResolvedSurface& ready_source,
                    const ResolvedSurface& ready_destination) {
                    execute_quad_copy(resumed, context_api, ready_source,
                        ready_destination, position_values, texture_values,
                        perspective, true, cpu, stage);
                }, false, cpu_draw_lease(cpu));
            return;
        }
        if (imported != SurfaceStore::CpuImportResult::Completed) {
            call.set_return(mbx2d_abi::failure);
            return;
        }
    }
    const auto recover = [&](QuadStage next) -> UserlandHleCall::Continuation {
        return [this, context_api, source = *source, destination = *destination,
                   position_values, texture_values, perspective, next](
                   UserlandHleCall& resumed) {
            execute_quad_copy(resumed, context_api, source, destination,
                position_values, texture_values, perspective, true, { }, next);
        };
    };
    if (!cpu && stage == QuadStage::Rectangle &&
        axis_aligned_affine && host_rotation && destination_unclipped &&
        destination_integral && !source->cpu_frame && host_graphics_->accelerated() &&
        source->host_surface &&
        destination->host_surface && source->backing && destination->backing &&
        source->backing->pixel_format == surface_pixel_format_bgra &&
        destination->backing->pixel_format == surface_pixel_format_bgra &&
        composite_mode &&
        source_left <= std::numeric_limits<std::int32_t>::max() &&
        source_top <= std::numeric_limits<std::int32_t>::max() &&
        left <= std::numeric_limits<std::int32_t>::max() &&
        top <= std::numeric_limits<std::int32_t>::max() &&
        static_cast<std::uint64_t>(source_width) <=
            std::numeric_limits<std::uint32_t>::max() &&
        static_cast<std::uint64_t>(source_height) <=
            std::numeric_limits<std::uint32_t>::max() &&
        static_cast<std::uint64_t>(width) <=
            std::numeric_limits<std::uint32_t>::max() &&
        static_cast<std::uint64_t>(height) <=
            std::numeric_limits<std::uint32_t>::max()) {
        copy_native(call, context_api, *source, *destination,
            { static_cast<std::int32_t>(source_left),
                static_cast<std::int32_t>(source_top),
                static_cast<std::uint32_t>(source_width),
                static_cast<std::uint32_t>(source_height) },
            { static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height) },
            *composite_mode, state.blend.global_alpha, *host_rotation,
            recover(QuadStage::Quad), true);
        return;
    }
    const auto ordered_geometry = triangles_share_only_diagonal(*positions);
    const auto host_ready =
        !source->cpu_frame && host_graphics_->accelerated() &&
        source->host_surface && destination->host_surface && source->backing &&
        destination->backing &&
        source->backing->pixel_format == surface_pixel_format_bgra &&
        destination->backing->pixel_format == surface_pixel_format_bgra;
    const bool native_quad_attempted = stage == QuadStage::Software;
    if (!cpu && stage != QuadStage::Software && ordered_geometry &&
        host_ready && composite_mode &&
        left <= std::numeric_limits<std::int32_t>::max() &&
        top <= std::numeric_limits<std::int32_t>::max() &&
        static_cast<std::uint64_t>(width) <=
            std::numeric_limits<std::uint32_t>::max() &&
        static_cast<std::uint64_t>(height) <=
            std::numeric_limits<std::uint32_t>::max()) {
        std::array<HostTexturedVertex, 4> quad { };
        for (std::size_t index = 0; index < quad.size(); ++index) {
            quad[index] = { { (*positions)[index].x, (*positions)[index].y },
                { (*texture)[index].x, (*texture)[index].y },
                perspective[index] };
        }
        copy_quad_native(call, context_api, *source, *destination, quad,
            { static_cast<std::int32_t>(source_left),
                static_cast<std::int32_t>(source_top),
                static_cast<std::uint32_t>(source_width),
                static_cast<std::uint32_t>(source_height) },
            { static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height) },
            *composite_mode, state.blend.global_alpha,
            recover(QuadStage::Software));
        return;
    }
    if (!source->cpu_frame && !cpu) {
        performance_counters().record_hle(native_quad_attempted
                                              ? "QuadCopy.NativeQuadDeclined"
                                              : "QuadCopy.NativeQuadIneligible",
            0);
    }
    if (!cpu && (source->backing || source->framebuffer) &&
        (source->framebuffer || destination->framebuffer ||
            (destination->host_surface && destination->core_surface_id))) {
        prepare_cpu_draw(call, context_api, *source, *destination,
            static_cast<std::size_t>(width * height) * sizeof(std::uint32_t),
            [this, context_api, position_values, texture_values, perspective,
                stage](UserlandHleCall& resumed,
                const std::optional<ResolvedSurface>& ready_source,
                const ResolvedSurface& ready_destination,
                std::shared_ptr<CpuDraw> ready) {
                execute_quad_copy(resumed, context_api, *ready_source,
                    ready_destination, position_values, texture_values,
                    perspective, true, std::move(ready), stage);
            });
        return;
    }
    if (!source->cpu_frame && source->core_surface_id != 0 &&
        source->host_surface && source->host_surface->gpu_generation() != 0) {
        import_source(call, context_api, *source, *destination,
            [this, context_api, position_values, texture_values, perspective,
                    cpu, stage](
                UserlandHleCall& resumed, const ResolvedSurface& ready_source,
                const ResolvedSurface& ready_destination) {
                execute_quad_copy(resumed, context_api, ready_source,
                    ready_destination, position_values, texture_values,
                    perspective, true, cpu, stage);
            },
            true, cpu_draw_lease(cpu));
        return;
    }
    performance_counters().record_hle("QuadCopy.Software", 0);
    const auto source_pixels = read_region(
        *source, source_left, source_top, source_width, source_height, call);
    if (!source_pixels) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    const auto destination_pixels = cpu
        ? std::optional<std::vector<std::uint32_t>> { }
        : read_region(*destination, left, top, width, height, call);
    if (!cpu && !destination_pixels) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    std::vector<std::uint32_t> sampled = cpu
        ? std::vector<std::uint32_t>(static_cast<std::size_t>(width * height))
        : *destination_pixels;
    std::vector<bool> covered;
    if (axis_aligned_affine) {
        const auto inverse_width = 1.0F / (right_value - left_value);
        const auto inverse_height = 1.0F / (bottom_value - top_value);
        for (std::int64_t y = 0; y < height; ++y) {
            const auto vertical =
                (static_cast<float>(top + y) + 0.5F - top_value) *
                inverse_height;
            const auto row_u = (*texture)[0].x +
                               vertical * ((*texture)[3].x - (*texture)[0].x);
            const auto row_v = (*texture)[0].y +
                               vertical * ((*texture)[3].y - (*texture)[0].y);
            for (std::int64_t x = 0; x < width; ++x) {
                const auto horizontal =
                    (static_cast<float>(left + x) + 0.5F - left_value) *
                    inverse_width;
                const auto u =
                    row_u + horizontal * ((*texture)[1].x - (*texture)[0].x);
                const auto v =
                    row_v + horizontal * ((*texture)[1].y - (*texture)[0].y);
                const auto source_x = std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::floor(u)), source_left,
                    source_right - 1);
                const auto source_y = std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::floor(v)), source_top,
                    source_bottom - 1);
                sampled[static_cast<std::size_t>(y * width + x)] =
                    (*source_pixels)[static_cast<std::size_t>(
                        (source_y - source_top) * source_width + source_x -
                        source_left)];
            }
        }
    } else {
        if (!cpu)
            covered.assign(static_cast<std::size_t>(width * height), false);
        for (std::int64_t y = 0; y < height; ++y) {
            for (std::int64_t x = 0; x < width; ++x) {
                const Point point { static_cast<float>(left + x) + 0.5F,
                    static_cast<float>(top + y) + 0.5F };
                const auto triangle = sample_quad(*positions, point);
                if (!triangle)
                    continue;
                const auto texture_point =
                    interpolate_perspective(*texture, perspective, *triangle);
                if (!texture_point)
                    continue;
                const auto u = texture_point->x;
                const auto v = texture_point->y;
                const auto source_x = std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::floor(u)), source_left,
                    source_right - 1);
                const auto source_y = std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(std::floor(v)), source_top,
                    source_bottom - 1);
                const auto destination_index =
                    static_cast<std::size_t>(y * width + x);
                sampled[destination_index] =
                    (*source_pixels)[static_cast<std::size_t>(
                        (source_y - source_top) * source_width + source_x -
                        source_left)];
                if (!cpu)
                    covered[destination_index] = true;
            }
        }
    }

    if (cpu) {
        write_cpu_draw(call, std::move(cpu),
            { static_cast<std::int32_t>(left), static_cast<std::int32_t>(top),
                static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height) },
            std::move(sampled), axis_aligned_affine ? std::nullopt
                : std::optional { QuadCoverage { position_values, perspective } });
        return;
    }
    auto pixels =
        composite(state, *destination, left, top, width, height, sampled, call);
    if (!pixels) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    if (!axis_aligned_affine) {
        for (std::size_t index = 0; index < covered.size(); ++index) {
            if (!covered[index])
                (*pixels)[index] = (*destination_pixels)[index];
        }
    }
    call.set_return(
        write_region(*destination, left, top, width, height, *pixels, call)
            ? mbx2d_abi::success
            : mbx2d_abi::failure);
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

} // namespace ilemu
