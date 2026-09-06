#include "kernel/core_animation_software_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/fixed_point_texture_sampler.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <vector>

namespace ilemu {
namespace {

    struct Fixed16SoftwareSamplerArm32Profile {
        static constexpr std::uint32_t texture_offset = 0U;
        static constexpr std::uint32_t coordinate_offset = 0x20U;
        static constexpr std::uint32_t texture_words = 6U;
        static constexpr std::uint32_t maximum_span = 16384U;
    };

    bool sample_scanline(UserlandHleCall& call, bool linear, bool opaque)
    {
        using Profile = Fixed16SoftwareSamplerArm32Profile;
        const auto count = call.argument(1);
        if (count == 0U)
            return true;
        if constexpr (std::endian::native != std::endian::little)
            return false;
        if (count > Profile::maximum_span)
            return false;
        auto& memory = call.memory();
        const auto data = call.argument(0);
        if (data > std::numeric_limits<std::uint32_t>::max() - 0x2fU)
            return false;
        const auto texture = memory.read32(data + Profile::texture_offset);
        std::array<std::uint32_t, 4> coordinates;
        std::array<std::uint32_t, Profile::texture_words> image;
        if (!texture || *texture >
                            std::numeric_limits<std::uint32_t>::max() - 23U ||
            !memory.copy_out(data + Profile::coordinate_offset,
                std::as_writable_bytes(std::span { coordinates })) ||
            !memory.copy_out(*texture,
                std::as_writable_bytes(std::span { image })) ||
            coordinates[3] != 0U) {
            return false;
        }
        // Horizontal spans only need one or two source rows. Other affine
        // directions retain the firmware sampler instead of copying an image
        // for each span. No persistent texture cache can hide guest writes.
        const auto maximum_x = std::bit_cast<std::int32_t>(image[4]);
        const auto maximum_y = std::bit_cast<std::int32_t>(image[5]);
        if (maximum_x < 0 || maximum_y < 0)
            return false;
        const auto width = (image[4] >> 16U) + 1U;
        if (width > Profile::maximum_span || image[1] < width * 4U)
            return false;
        const auto clamp_y = [maximum_y](std::uint32_t value) {
            return static_cast<std::uint32_t>(std::clamp(
                std::bit_cast<std::int32_t>(value), 0, maximum_y));
        };
        const auto top =
            clamp_y(coordinates[2] - (linear ? 0x8000U : 0U));
        const auto bottom =
            linear ? clamp_y(coordinates[2] + 0x8000U) : top;
        std::vector<std::uint32_t> first_row(width), second_row;
        const auto read_row = [&](std::uint32_t coordinate, auto& row) {
            const auto address = static_cast<std::uint64_t>(image[0]) +
                                 (coordinate >> 16U) *
                                     static_cast<std::uint64_t>(image[1]);
            const auto destination =
                static_cast<std::uint64_t>(call.argument(2));
            if (address < destination + count * 4U &&
                destination < address + width * 4U)
                return false;
            return address + width * 4U <=
                       static_cast<std::uint64_t>(
                           std::numeric_limits<std::uint32_t>::max()) +
                           1U &&
                   memory.copy_out(static_cast<std::uint32_t>(address),
                       std::as_writable_bytes(std::span { row }));
        };
        if (!read_row(top, first_row))
            return false;
        if ((bottom >> 16U) != (top >> 16U)) {
            second_row.resize(width);
            if (!read_row(bottom, second_row))
                return false;
        }
        std::vector<std::uint32_t> result(count);
        FixedPointTextureSampler::sample(first_row,
            second_row.empty() ? first_row : second_row, coordinates[0],
            coordinates[1], maximum_x, (top >> 8U) & 0xffU, linear, opaque,
            result);
        return memory.copy_in(
            call.argument(2), std::as_bytes(std::span { result }));
    }

} // namespace

void register_core_animation_software_hle(UserlandHleRegistry& registry)
{
    for (const auto opaque : { false, true }) {
        for (const auto linear : { false, true }) {
            const auto symbol =
                std::string { "__ZN2CA3OGL2SW13image_samplerINS1_6Format10" } +
                (opaque ? "XRGB8" : "ARGB8") +
                "_HostELb1ELb1ELb0ELb" + (linear ? "1" : "0") +
                "EEEvPKNS1_11SamplerDataEjPj";
            registry.register_function("/QuartzCore.framework/QuartzCore",
                symbol, [linear, opaque](UserlandHleCall& call) {
                    if (!sample_scanline(call, linear, opaque))
                        call.resume_original_persistently();
                    else
                        call.set_return(0U);
                });
        }
    }
}

} // namespace ilemu
