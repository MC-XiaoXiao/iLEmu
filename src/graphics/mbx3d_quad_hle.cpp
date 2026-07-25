#include "ilemu/mbx2d_hle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/mbx2d_abi.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

struct Point {
  float x{};
  float y{};
};

constexpr float edge_tolerance = 0.01F;
constexpr std::uint64_t maximum_quad_pixels = 16U * 1024U * 1024U;
constexpr std::uint64_t scene_extent_numerator = 3;
constexpr std::uint64_t scene_extent_denominator = 4;

bool nearly_equal(float left, float right) {
  return std::abs(left - right) <= edge_tolerance;
}

bool covers_scene_extent(std::int64_t width, std::int64_t height,
                         std::uint32_t destination_width,
                         std::uint32_t destination_height) {
  if (width <= 0 || height <= 0) return false;
  return static_cast<std::uint64_t>(width) * scene_extent_denominator >=
             static_cast<std::uint64_t>(destination_width) *
                 scene_extent_numerator &&
         static_cast<std::uint64_t>(height) * scene_extent_denominator >=
             static_cast<std::uint64_t>(destination_height) *
                 scene_extent_numerator;
}

std::optional<std::array<Point, 4>> read_quad(AddressSpace &memory,
                                              std::uint32_t address) {
  if (address == 0 || address > std::numeric_limits<std::uint32_t>::max() -
                                    7U * sizeof(std::uint32_t)) {
    return std::nullopt;
  }
  std::array<Point, 4> result{};
  for (std::size_t vertex = 0; vertex < result.size(); ++vertex) {
    const auto x =
        memory.read32(address + static_cast<std::uint32_t>(vertex * 8U));
    const auto y =
        memory.read32(address + static_cast<std::uint32_t>(vertex * 8U + 4U));
    if (!x || !y)
      return std::nullopt;
    result[vertex] = {std::bit_cast<float>(*x), std::bit_cast<float>(*y)};
    if (!std::isfinite(result[vertex].x) || !std::isfinite(result[vertex].y)) {
      return std::nullopt;
    }
  }
  return result;
}

struct TriangleSample {
  std::array<std::size_t, 3> vertices{};
  std::array<float, 3> weights{};
};

std::optional<TriangleSample>
sample_triangle(const std::array<Point, 4> &quad,
                std::array<std::size_t, 3> vertices, Point point) {
  const auto &first = quad[vertices[0]];
  const auto &second = quad[vertices[1]];
  const auto &third = quad[vertices[2]];
  const auto denominator = (second.y - third.y) * (first.x - third.x) +
                           (third.x - second.x) * (first.y - third.y);
  if (std::abs(denominator) <= edge_tolerance)
    return std::nullopt;
  const auto first_weight = ((second.y - third.y) * (point.x - third.x) +
                             (third.x - second.x) * (point.y - third.y)) /
                            denominator;
  const auto second_weight = ((third.y - first.y) * (point.x - third.x) +
                              (first.x - third.x) * (point.y - third.y)) /
                             denominator;
  const auto third_weight = 1.0F - first_weight - second_weight;
  if (first_weight < -edge_tolerance || second_weight < -edge_tolerance ||
      third_weight < -edge_tolerance)
    return std::nullopt;
  return TriangleSample{vertices, {first_weight, second_weight, third_weight}};
}

std::optional<TriangleSample> sample_quad(const std::array<Point, 4> &quad,
                                          Point point) {
  if (const auto first = sample_triangle(quad, {0, 1, 2}, point))
    return first;
  return sample_triangle(quad, {0, 2, 3}, point);
}

float interpolate_triangle(const std::array<Point, 4> &values,
                           const TriangleSample &sample, bool x_axis) {
  float result = 0.0F;
  for (std::size_t index = 0; index < sample.vertices.size(); ++index) {
    const auto &value = values[sample.vertices[index]];
    result += sample.weights[index] * (x_axis ? value.x : value.y);
  }
  return result;
}

} // namespace

void Mbx2dHle::quad_color(UserlandHleCall &call) {
  const auto destination = resolve(state_.destination);
  const auto positions = read_quad(call.memory(), call.argument(0));
  if (!destination || !positions) {
    call.set_return(mbx2d_abi::failure);
    return;
  }

  auto left_value = (*positions)[0].x;
  auto right_value = left_value;
  auto top_value = (*positions)[0].y;
  auto bottom_value = top_value;
  for (const auto &position : *positions) {
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
      static_cast<std::int64_t>(std::ceil(bottom_value)), destination->height);
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
  if (covers_scene_extent(width, height, destination->width,
                          destination->height)) {
    prepare_destination_for_frame(
        call, state_, DamageRegion{left, top, right, bottom}, 0U);
  }
  const auto destination_pixels =
      read_region(*destination, left, top, width, height, call);
  if (!destination_pixels) {
    call.set_return(mbx2d_abi::failure);
    return;
  }

  const auto axis_aligned =
      nearly_equal((*positions)[0].x, left_value) &&
      nearly_equal((*positions)[0].y, top_value) &&
      nearly_equal((*positions)[1].x, right_value) &&
      nearly_equal((*positions)[1].y, top_value) &&
      nearly_equal((*positions)[2].x, right_value) &&
      nearly_equal((*positions)[2].y, bottom_value) &&
      nearly_equal((*positions)[3].x, left_value) &&
      nearly_equal((*positions)[3].y, bottom_value);
  std::vector<std::uint32_t> source_pixels(
      static_cast<std::size_t>(width * height), call.argument(1));
  std::vector<bool> covered;
  if (!axis_aligned) {
    covered.assign(source_pixels.size(), false);
    for (std::int64_t y = 0; y < height; ++y) {
      for (std::int64_t x = 0; x < width; ++x) {
        const auto index = static_cast<std::size_t>(y * width + x);
        covered[index] = sample_quad(
                             *positions,
                             Point{static_cast<float>(left + x) + 0.5F,
                                   static_cast<float>(top + y) + 0.5F})
                             .has_value();
      }
    }
  }

  auto pixels = composite(state_, *destination, left, top, width, height,
                          source_pixels, call);
  if (!pixels) {
    call.set_return(mbx2d_abi::failure);
    return;
  }
  if (!axis_aligned) {
    for (std::size_t index = 0; index < covered.size(); ++index) {
      if (!covered[index]) (*pixels)[index] = (*destination_pixels)[index];
    }
  }
  call.set_return(
      write_region(*destination, left, top, width, height, *pixels, call)
          ? mbx2d_abi::success
          : mbx2d_abi::failure);
}

void Mbx2dHle::quad_copy(UserlandHleCall &call) {
  const auto source = resolve(state_.source);
  const auto destination = resolve(state_.destination);
  const auto positions = read_quad(call.memory(), call.argument(0));
  const auto texture = read_quad(call.memory(), call.argument(1));
  if (!source || !destination || !positions || !texture) {
    call.set_return(mbx2d_abi::failure);
    return;
  }

  auto left_value = (*positions)[0].x;
  auto right_value = left_value;
  auto top_value = (*positions)[0].y;
  auto bottom_value = top_value;
  for (const auto &position : *positions) {
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

  auto left = static_cast<std::int64_t>(std::floor(left_value));
  auto right = static_cast<std::int64_t>(std::ceil(right_value));
  auto top = static_cast<std::int64_t>(std::floor(top_value));
  auto bottom = static_cast<std::int64_t>(std::ceil(bottom_value));
  left = std::max<std::int64_t>(left, 0);
  top = std::max<std::int64_t>(top, 0);
  right = std::min<std::int64_t>(right, destination->width);
  bottom = std::min<std::int64_t>(bottom, destination->height);
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

  float minimum_u = (*texture)[0].x;
  float maximum_u = minimum_u;
  float minimum_v = (*texture)[0].y;
  float maximum_v = minimum_v;
  for (const auto &coordinate : *texture) {
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
  const auto source_pixels = read_region(*source, source_left, source_top,
                                         source_width, source_height, call);
  const auto width = right - left;
  const auto height = bottom - top;
  if (!source_pixels ||
      static_cast<std::uint64_t>(width) >
          maximum_quad_pixels / static_cast<std::uint64_t>(height)) {
    call.set_return(mbx2d_abi::failure);
    return;
  }

  if (covers_scene_extent(width, height, destination->width,
                          destination->height)) {
    prepare_destination_for_frame(
        call, state_, DamageRegion{left, top, right, bottom},
        state_.source ? state_.source->surface : 0U);
  }
  const auto destination_pixels =
      read_region(*destination, left, top, width, height, call);
  if (!destination_pixels) {
    call.set_return(mbx2d_abi::failure);
    return;
  }
  std::vector<std::uint32_t> sampled = *destination_pixels;
  const auto axis_aligned_affine =
      nearly_equal((*positions)[0].x, left_value) &&
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
  std::vector<bool> covered;
  if (axis_aligned_affine) {
    const auto inverse_width = 1.0F / (right_value - left_value);
    const auto inverse_height = 1.0F / (bottom_value - top_value);
    for (std::int64_t y = 0; y < height; ++y) {
      const auto vertical =
          (static_cast<float>(top + y) + 0.5F - top_value) * inverse_height;
      const auto row_u =
          (*texture)[0].x + vertical * ((*texture)[3].x - (*texture)[0].x);
      const auto row_v =
          (*texture)[0].y + vertical * ((*texture)[3].y - (*texture)[0].y);
      for (std::int64_t x = 0; x < width; ++x) {
        const auto horizontal =
            (static_cast<float>(left + x) + 0.5F - left_value) * inverse_width;
        const auto u =
            row_u + horizontal * ((*texture)[1].x - (*texture)[0].x);
        const auto v =
            row_v + horizontal * ((*texture)[1].y - (*texture)[0].y);
        const auto source_x =
            std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(u)),
                                     source_left, source_right - 1);
        const auto source_y =
            std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(v)),
                                     source_top, source_bottom - 1);
        sampled[static_cast<std::size_t>(y * width + x)] =
            (*source_pixels)[static_cast<std::size_t>(
                (source_y - source_top) * source_width + source_x -
                source_left)];
      }
    }
  } else {
    covered.assign(static_cast<std::size_t>(width * height), false);
    for (std::int64_t y = 0; y < height; ++y) {
      for (std::int64_t x = 0; x < width; ++x) {
        const Point point{static_cast<float>(left + x) + 0.5F,
                          static_cast<float>(top + y) + 0.5F};
        const auto triangle = sample_quad(*positions, point);
        if (!triangle)
          continue;
        const auto u = interpolate_triangle(*texture, *triangle, true);
        const auto v = interpolate_triangle(*texture, *triangle, false);
        const auto source_x = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::floor(u)), source_left,
            source_right - 1);
        const auto source_y = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(std::floor(v)), source_top,
            source_bottom - 1);
        const auto destination_index =
            static_cast<std::size_t>(y * width + x);
        sampled[destination_index] = (*source_pixels)[static_cast<std::size_t>(
            (source_y - source_top) * source_width + source_x - source_left)];
        covered[destination_index] = true;
      }
    }
  }

  auto pixels =
      composite(state_, *destination, left, top, width, height, sampled, call);
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
}

} // namespace ilemu
