// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "vimage_tent_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/firmware_routine.hpp"
#include "foundation/userland_hle.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace ilemu {
namespace {

    // Hook before the column loop, so a fallback pays for one trap per
    // invocation of the loop.
    constexpr std::uint32_t tent_update_size = 0x38U;
    constexpr FirmwareRoutine tent_update { tent_update_size,
        { 0x4d, 0xc5, 0x20, 0x53, 0xc9, 0xac, 0xd8, 0xa7, 0x90, 0xc7, 0x63,
            0x93, 0xc9, 0x47, 0x48, 0x2e, 0x9c, 0x1e, 0x6f, 0x13, 0xf2,
            0xa4, 0xad, 0x0f, 0xbe, 0x67, 0x06, 0x75, 0x9e, 0x2f, 0x87,
            0x73 } };
    constexpr std::uint32_t tent_row_size = 0x14aU;
    constexpr FirmwareRoutine tent_row { tent_row_size,
        { 0x4d, 0x47, 0x49, 0x7e, 0x55, 0x43, 0x64, 0x06, 0xe6, 0x83, 0x59,
            0x45, 0x43, 0xe5, 0x22, 0xe3, 0x27, 0x7e, 0x5d, 0x67, 0xe4, 0x2b,
            0x2a, 0x5d, 0xa4, 0x91, 0xa2, 0xc4, 0xf3, 0x92, 0x24, 0x94 } };
    constexpr auto guest_address_limit = std::uint64_t { 1 } << 32U;
    constexpr std::uint32_t channels = 4U;
    constexpr std::uint32_t live_frame_size = 140U;
    constexpr std::uint32_t source_stack_offset = 84U;

    [[nodiscard]] bool overlaps(std::uint32_t address, std::uint64_t size,
        std::uint32_t other_address, std::uint64_t other_size)
    {
        return address < other_address + other_size &&
               other_address < address + size;
    }

    [[nodiscard]] std::uint32_t load_word(
        std::span<const std::byte> bytes, std::size_t offset)
    {
        std::uint32_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    [[nodiscard]] bool try_output_tent_row(UserlandHleCall& call)
    {
        if constexpr (std::endian::native != std::endian::little)
            return false;

        auto& memory = call.memory();
        auto& registers = call.cpu().registers();
        const auto sp = registers[13];
        const auto scratch = registers[11];
        if (((sp | scratch) & 3U) != 0U ||
            static_cast<std::uint64_t>(sp) + live_frame_size >
                guest_address_limit)
            return false;
        const auto frame = memory.try_read_private_data(sp, live_frame_size);
        if (!frame)
            return false;
        const auto radius = load_word(*frame, 0x68U);
        const auto descriptor = load_word(*frame, 0x50U);
        const auto destination = load_word(*frame, 0x60U);
        const auto bias = load_word(*frame, 0x58U);
        const auto divisor = load_word(*frame, 0x5cU);
        if (radius > 127U || registers[3] != radius ||
            load_word(*frame, 0x48U) != radius * 16U ||
            load_word(*frame, 0x4cU) == 0U || divisor == 0U ||
            (descriptor & 3U) != 0U ||
            static_cast<std::uint64_t>(descriptor) + 16U >
                guest_address_limit ||
            overlaps(sp, live_frame_size, descriptor, 16U))
            return false;
        const auto width_bytes =
            memory.try_read_private_data(descriptor + 8U, 4U);
        if (!width_bytes)
            return false;
        const auto width = load_word(*width_bytes, 0U);
        if (width < 16U || width > 4096U)
            return false;
        constexpr std::uint32_t pixel_size = channels * sizeof(std::uint32_t);
        const auto scratch_size = (width + 2U * radius) * pixel_size;
        const auto row_size = width * channels;
        if (static_cast<std::uint64_t>(scratch) + scratch_size >
                guest_address_limit ||
            static_cast<std::uint64_t>(destination) + row_size >
                guest_address_limit ||
            overlaps(scratch, scratch_size, sp, live_frame_size) ||
            overlaps(destination, row_size, scratch, scratch_size) ||
            overlaps(destination, row_size, sp, live_frame_size) ||
            overlaps(destination, row_size, descriptor, 16U))
            return false;
        const auto source = memory.try_read_private_data(scratch, scratch_size);
        if (!source)
            return false;
        const auto sample = [&](std::uint32_t pixel, std::size_t channel) {
            return load_word(
                *source, pixel * pixel_size + channel * sizeof(std::uint32_t));
        };
        constexpr std::array<std::size_t, channels> sum_registers { 8U, 12U,
            10U, 9U };
        std::array<std::uint32_t, channels> sums, differences { };
        for (std::size_t channel = 0; channel < channels; ++channel) {
            sums[channel] = registers[sum_registers[channel]];
            for (std::uint32_t tap = 0; tap <= radius; ++tap)
                differences[channel] +=
                    sample(1U + radius + tap, channel) - sample(tap, channel);
        }
        std::vector<std::byte> output((width - 1U) * channels);
        std::uint32_t quotient = 0U;
        for (std::uint32_t pixel = 1U; pixel < width; ++pixel) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                sums[channel] += differences[channel];
                quotient = (sums[channel] + bias) / divisor;
                output[(pixel - 1U) * channels + channel] =
                    static_cast<std::byte>(quotient);
                // Slide the two adjacent windows instead of summing their
                // radius again for every output pixel. All arithmetic wraps
                // exactly like the guest's uint32 add/sub instructions.
                if (pixel + 1U < width) {
                    differences[channel] +=
                        sample(pixel + 2U * radius + 1U, channel) -
                        2U * sample(pixel + radius, channel) +
                        sample(pixel - 1U, channel);
                }
            }
        }
        const auto saved_scratch = scratch + (width - 2U) * pixel_size;
        const auto saved_bytes =
            std::as_bytes(std::span { &saved_scratch, 1U });
        const std::array writes {
            AddressSpace::CopyInOperation { destination + channels, output },
            AddressSpace::CopyInOperation { sp + 0x64U, saved_bytes },
        };
        if (!memory.try_copy_to_private_data(writes))
            return false;

        registers[0] = registers[4] = width - 1U;
        registers[1] = quotient;
        registers[2] = ((width - 1U) * channels) | 2U;
        registers[3] = radius;
        registers[5] = scratch + (width - 1U) * pixel_size;
        registers[6] = destination;
        for (std::size_t channel = 0; channel < channels; ++channel)
            registers[sum_registers[channel]] = sums[channel];
        registers[14] = radius * pixel_size;
        call.cpu().set_cpsr((call.cpu().cpsr() & 0x0fffffffU) | 0x60000000U);
        return true;
    }

    [[nodiscard]] bool try_update_tent(UserlandHleCall& call)
    {
        if constexpr (std::endian::native != std::endian::little)
            return false;

        auto& memory = call.memory();
        auto& registers = call.cpu().registers();
        const auto sp = registers[13];
        const auto descriptor = registers[10];
        const auto accumulator = registers[11];
        const auto stride = registers[3];
        const auto radius = registers[14];
        if (registers[0] != 0U || stride == 0U || radius > 127U ||
            ((sp | descriptor | accumulator) & 3U) != 0U ||
            static_cast<std::uint64_t>(sp) + live_frame_size >
                guest_address_limit ||
            static_cast<std::uint64_t>(descriptor) + 16U > guest_address_limit)
            return false;

        const auto frame = memory.try_read_private_data(
            sp, source_stack_offset + sizeof(std::uint32_t));
        const auto buffer = memory.try_read_private_data(descriptor + 8U, 8U);
        if (!frame || !buffer)
            return false;

        std::uint32_t kernel_height, source, width, current_stride;
        std::memcpy(&kernel_height, frame->data(), sizeof(kernel_height));
        std::memcpy(&source, frame->data() + source_stack_offset, sizeof(source));
        std::memcpy(&width, buffer->data(), sizeof(width));
        std::memcpy(&current_stride, buffer->data() + sizeof(width),
            sizeof(current_stride));
        if (kernel_height != 2U * radius + 1U || width < 16U ||
            width > 4096U || current_stride != stride)
            return false;

        const auto columns = width * channels;
        const auto source_size =
            static_cast<std::uint64_t>(kernel_height) * stride + columns;
        const auto accumulator_size = columns * sizeof(std::uint32_t);
        if (stride < columns || stride > 2U * columns ||
            source_size > 2U * 1024U * 1024U ||
            source + source_size > guest_address_limit ||
            overlaps(accumulator, accumulator_size, source, source_size) ||
            overlaps(accumulator, accumulator_size, sp, live_frame_size) ||
            overlaps(accumulator, accumulator_size, descriptor, 16U))
            return false;

        const auto pixels = memory.try_read_private_data(
            source, static_cast<std::size_t>(source_size));
        auto sums = memory.try_read_private_data(accumulator, accumulator_size);
        if (!pixels || !sums)
            return false;

        // The guest walks inward from both ends of each byte column. Visit
        // whole rows so the host can vectorize the same unsigned sums.
        for (std::uint32_t row = 0; row <= radius; ++row) {
            const auto* bottom = pixels->data() + (kernel_height - row) * stride;
            const auto* top = pixels->data() + row * stride;
            for (std::uint32_t column = 0; column < columns; ++column) {
                auto* slot = sums->data() + column * sizeof(std::uint32_t);
                std::uint32_t value;
                std::memcpy(&value, slot, sizeof(value));
                value += std::to_integer<std::uint32_t>(bottom[column]) -
                         std::to_integer<std::uint32_t>(top[column]);
                std::memcpy(slot, &value, sizeof(value));
            }
        }
        const AddressSpace::CopyInOperation write { accumulator, *sums };
        if (!memory.try_copy_to_private_data({ &write, 1U }))
            return false;

        registers[0] = columns;
        registers[1] = width;
        std::memcpy(&registers[2], sums->data() + accumulator_size -
                sizeof(std::uint32_t), sizeof(std::uint32_t));
        registers[4] =
            std::to_integer<std::uint32_t>((*pixels)[radius * stride + columns - 1U]);
        registers[5] = source + (radius + 1U) * stride;
        registers[6] = radius + 1U;
        registers[9] = kernel_height;
        // The final comparison is columns == width * 4.
        call.cpu().set_cpsr((call.cpu().cpsr() & 0x0fffffffU) | 0x60000000U);
        return true;
    }

    void convolve_tent(UserlandHleCall& call)
    {
        constexpr std::uint32_t thumb_state = 1U << 5U;
        constexpr std::uint32_t big_endian = 1U << 9U;
        const auto state = call.cpu().cpsr();
        auto& memory = call.memory();
        const AddressSpace::ExclusiveAccess access { memory };
        // In Thumb state the SVC advances the PC by its own 16-bit width.
        const auto entry = call.cpu().registers()[15] - 2U;
        if (call.cpu().guest_debugging_enabled() ||
            (state & (thumb_state | big_endian)) != thumb_state) {
            call.resume_original();
            return;
        }
        const auto update = tent_update.matches(memory, entry);
        if (!update && !tent_row.matches(memory, entry)) {
            call.resume_original();
            return;
        }
        if (!(update ? try_update_tent(call) : try_output_tent_row(call))) {
            call.resume_original_persistently();
            return;
        }
        call.resume_original_at(
            (entry + (update ? tent_update_size : tent_row_size)) | 1U);
    }

} // namespace

void register_vimage_tent_hle(UserlandHleRegistry& registry)
{
    registry.register_thumb_instructions(
        "/System/Library/Frameworks/Accelerate.framework/Frameworks/"
        "vImage.framework/vImage",
        "_sTentConvolve_ARGB8888", { tent_update, tent_row }, convolve_tent);
}

} // namespace ilemu
