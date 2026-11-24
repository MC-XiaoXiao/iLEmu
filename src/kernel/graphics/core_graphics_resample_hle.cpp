// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "core_graphics_resample_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/firmware_routine.hpp"
#include "foundation/userland_hle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace ilemu {
namespace {

    constexpr std::size_t channels = 4;
    constexpr std::size_t output_pixel_size = channels * sizeof(std::uint32_t);
    constexpr std::size_t filter_header_size = 8;
    constexpr std::size_t frame_size = 52;
    constexpr std::size_t stack_arguments_size = 16;
    constexpr std::size_t maximum_filters = 4096;
    constexpr std::size_t maximum_taps = 256;
    constexpr std::size_t maximum_rows = 256;
    constexpr std::size_t maximum_work = 4U * 1024U * 1024U;
    constexpr std::size_t maximum_buffer_size = 4U * 1024U * 1024U;

    template <typename Integer>
    [[nodiscard]] Integer load_little_endian(
        std::span<const std::byte> bytes, std::size_t offset)
    {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < sizeof(Integer); ++i)
            value |= std::to_integer<std::uint32_t>(bytes[offset + i])
                     << (8U * i);
        using Unsigned = std::make_unsigned_t<Integer>;
        return std::bit_cast<Integer>(static_cast<Unsigned>(value));
    }

    struct GuestRange {
        std::uint32_t address;
        std::size_t size;

        [[nodiscard]] bool overlaps(GuestRange other) const
        {
            const auto end = static_cast<std::uint64_t>(address) + size;
            const auto other_end =
                static_cast<std::uint64_t>(other.address) + other.size;
            return address < other_end && other.address < end;
        }
    };

    [[nodiscard]] std::optional<GuestRange> guest_range(
        std::int64_t address, std::size_t size)
    {
        if (address <= 0 || static_cast<std::uint64_t>(address) + size >
                                (std::uint64_t { 1 } << 32U))
            return std::nullopt;
        return GuestRange { static_cast<std::uint32_t>(address), size };
    }

    // Includes the PC-relative vector constant, which requires a 16-byte
    // aligned entry.
    constexpr FirmwareRoutine horizontal_resample { 0x1b0U,
        { 0xc2, 0x78, 0x63, 0x6a, 0x87, 0x17, 0x09, 0xaa, 0xa3, 0x2d, 0xf1,
            0xca, 0x63, 0x12, 0x71, 0x3a, 0xbc, 0xe0, 0xe5, 0xfb, 0x27, 0xf3,
            0x23, 0x88, 0x4e, 0x86, 0xd4, 0xc8, 0x8b, 0xbc, 0xf3, 0x5a } };

    struct Filter {
        std::int32_t first_pixel;
        std::uint32_t taps;
        std::size_t coefficient_index;
    };

    [[nodiscard]] bool convolve_row(std::span<const std::byte> source,
        std::span<std::byte> output, std::int64_t first_pixel,
        std::span<const Filter> filters,
        std::span<const std::int16_t> coefficients)
    {
        bool saturated = false;
        for (const auto& filter : filters) {
            // At most 256 signed 16-bit weights times unsigned 8-bit
            // samples, plus the guest's rounding bias, fit in int32_t.
            std::array<std::int32_t, channels> sums { 8192, 8192, 8192, 8192 };
            auto source_offset =
                static_cast<std::size_t>(filter.first_pixel - first_pixel) *
                channels;
            for (const auto weight :
                coefficients.subspan(filter.coefficient_index, filter.taps)) {
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    sums[channel] +=
                        weight * std::to_integer<std::int32_t>(
                                     source[source_offset + channel]);
                }
                source_offset += channels;
            }
            for (std::size_t channel = 0; channel < channels; ++channel) {
                const auto shifted = sums[channel] >> 14U;
                const auto clamped = std::clamp(shifted, 0, 255);
                saturated |= shifted != clamped;
                // This horizontal pass stores each byte in a 32-bit
                // intermediate lane for the subsequent vertical filter.
                output[channel * sizeof(std::uint32_t)] =
                    static_cast<std::byte>(clamped);
            }
            output = output.subspan(output_pixel_size);
        }
        return saturated;
    }

    // r0-r3: filter records, reserved taps per record, unused, output width.
    // Stack: output row pointers, pointer to source base, pointer to source
    // stride, row count. Additional arguments are unused by this
    // implementation.
    [[nodiscard]] bool try_resample(UserlandHleCall& call)
    {
        auto& memory = call.memory();
        auto& cpu = call.cpu();
        auto& registers = cpu.registers();
        const auto width = call.argument(3);
        const auto reserved_taps = call.argument(1);
        const auto sp = registers[13];
        if (width == 0U || width > maximum_filters || reserved_taps == 0U ||
            reserved_taps > maximum_taps || (sp & 3U) != 0U || sp < frame_size)
            return false;

        const auto frame_address = sp - static_cast<std::uint32_t>(frame_size);
        const auto stack =
            guest_range(frame_address, frame_size + stack_arguments_size);
        if (!stack || !memory.accessible(frame_address, frame_size,
                          MemoryPermission::Read | MemoryPermission::Write))
            return false;
        const GuestRange frame { frame_address, frame_size };
        const auto stack_bytes =
            memory.try_read_private_data(stack->address, stack->size);
        if (!stack_bytes)
            return false;
        const auto stack_arguments =
            std::span { *stack_bytes }.subspan(frame_size);

        const auto rows =
            load_little_endian<std::uint32_t>(stack_arguments, 12);
        const auto row_bytes = width * output_pixel_size;
        if (rows == 0U || rows > maximum_rows ||
            rows * row_bytes > maximum_buffer_size)
            return false;
        const auto record_size =
            filter_header_size + sizeof(std::int16_t) * reserved_taps;
        const std::array inputs {
            guest_range(call.argument(0), width * record_size),
            guest_range(load_little_endian<std::uint32_t>(stack_arguments, 0),
                rows * sizeof(std::uint32_t)),
            guest_range(load_little_endian<std::uint32_t>(stack_arguments, 4),
                sizeof(std::uint32_t)),
            guest_range(load_little_endian<std::uint32_t>(stack_arguments, 8),
                sizeof(std::uint32_t)),
        };
        if (std::ranges::any_of(inputs, [&](const auto& input) {
                return !input || (input->address & 3U) != 0U ||
                       input->overlaps(frame);
            }))
            return false;
        const auto records =
            memory.try_read_private_data(inputs[0]->address, inputs[0]->size);
        const auto row_pointers =
            memory.try_read_private_data(inputs[1]->address, inputs[1]->size);
        const auto base_bytes =
            memory.try_read_private_data(inputs[2]->address, inputs[2]->size);
        const auto stride_bytes =
            memory.try_read_private_data(inputs[3]->address, inputs[3]->size);
        if (!records || !row_pointers || !base_bytes || !stride_bytes)
            return false;
        const auto base = load_little_endian<std::uint32_t>(*base_bytes, 0);
        const auto stride = load_little_endian<std::int32_t>(*stride_bytes, 0);
        if ((base & 3U) != 0U ||
            (stride % static_cast<std::int32_t>(channels)) != 0)
            return false;

        std::vector<GuestRange> destinations;
        destinations.reserve(rows);
        for (std::size_t row = 0; row < rows; ++row) {
            const auto address = load_little_endian<std::uint32_t>(
                *row_pointers, row * sizeof(std::uint32_t));
            const auto range = guest_range(address, row_bytes);
            if (!range || (address & 3U) != 0U || range->overlaps(*stack) ||
                std::ranges::any_of(inputs,
                    [&](const auto& input) {
                        return range->overlaps(*input);
                    }) ||
                std::ranges::any_of(destinations, [&](const auto& previous) {
                    return range->overlaps(previous);
                }))
                return false;
            destinations.push_back(*range);
        }

        std::vector<Filter> filters;
        filters.reserve(width);
        std::vector<std::int16_t> coefficients;
        coefficients.reserve(
            std::min<std::size_t>(width * reserved_taps, maximum_work / rows));
        std::int64_t first_pixel = std::numeric_limits<std::int32_t>::max();
        std::int64_t end_pixel = std::numeric_limits<std::int32_t>::min();
        for (std::size_t offset = 0; offset < records->size();
            offset += record_size) {
            const auto first =
                load_little_endian<std::int32_t>(*records, offset);
            const auto taps =
                load_little_endian<std::uint32_t>(*records, offset + 4);
            if (taps > reserved_taps ||
                coefficients.size() + taps > maximum_work / rows)
                return false;
            filters.push_back({ first, taps, coefficients.size() });
            if (taps == 0U)
                continue;
            first_pixel = std::min<std::int64_t>(first_pixel, first);
            end_pixel =
                std::max(end_pixel, static_cast<std::int64_t>(first) + taps);
            for (std::size_t tap = 0; tap < taps; ++tap) {
                const auto weight =
                    offset + filter_header_size + tap * sizeof(std::int16_t);
                coefficients.push_back(
                    load_little_endian<std::int16_t>(*records, weight));
            }
        }

        const auto source_size =
            (end_pixel - first_pixel) * static_cast<std::int64_t>(channels);
        if (source_size <= 0 ||
            source_size > static_cast<std::int64_t>(maximum_buffer_size / rows))
            return false;
        std::vector<std::byte> output(rows * row_bytes);
        std::vector<AddressSpace::CopyInOperation> writes;
        writes.reserve(rows + 1U);

        // The prologue saves two register groups above five locals. The row
        // loop overwrites the last three locals on every iteration.
        const auto last_source_row =
            base + static_cast<std::uint32_t>(stride) * (rows - 1U);
        const std::array frame_words {
            call.argument(0),
            width,
            rows,
            last_source_row + 32U,
            last_source_row,
            registers[8],
            registers[10],
            registers[11],
            registers[4],
            registers[5],
            registers[6],
            registers[7],
            registers[14],
        };
        static_assert(sizeof(frame_words) == frame_size);
        std::array<std::byte, frame_size> frame_bytes;
        for (std::size_t word = 0; word < frame_words.size(); ++word) {
            for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
                frame_bytes[word * sizeof(std::uint32_t) + byte] =
                    static_cast<std::byte>(frame_words[word] >> (8U * byte));
            }
        }
        writes.push_back({ frame_address, frame_bytes });

        bool saturated = false;
        for (std::size_t row = 0; row < rows; ++row) {
            const auto row_base = base + static_cast<std::int64_t>(stride) *
                                             static_cast<std::int64_t>(row);
            const auto range = guest_range(
                row_base + first_pixel * static_cast<std::int64_t>(channels),
                static_cast<std::size_t>(source_size));
            if (!range || range->overlaps(frame) ||
                std::ranges::any_of(destinations, [&](const auto& destination) {
                    return range->overlaps(destination);
                }))
                return false;
            const auto source =
                memory.try_read_private_data(range->address, range->size);
            if (!source)
                return false;
            const auto row_output =
                std::span { output }.subspan(row * row_bytes, row_bytes);
            saturated |= convolve_row(
                *source, row_output, first_pixel, filters, coefficients);
            writes.push_back({ destinations[row].address, row_output });
        }

        if (!memory.try_copy_to_private_data(writes))
            return false;

        constexpr std::uint32_t nzcv_mask = 0xf0000000U;
        constexpr std::uint32_t zero_and_carry = 0x60000000U;
        constexpr std::uint32_t fpscr_cumulative_saturation = 1U << 27U;
        cpu.set_cpsr((cpu.cpsr() & ~nzcv_mask) | zero_and_carry);
        if (saturated)
            cpu.set_fpscr(cpu.fpscr() | fpscr_cumulative_saturation);
        registers[9] = filters.back().taps % 2U;
        call.set_return(rows);
        return true;
    }

    void resample(UserlandHleCall& call)
    {
        constexpr std::uint32_t thumb_state = 1U << 5U;
        constexpr std::uint32_t big_endian = 1U << 9U;
        const auto state = call.cpu().cpsr();
        if (call.cpu().guest_debugging_enabled() ||
            (state & thumb_state) == 0U) {
            call.resume_original();
            return;
        }
        auto& memory = call.memory();
        const AddressSpace::ExclusiveAccess access { memory };
        const auto entry = call.symbol_address(call.symbol());
        if (!entry || call.cpu().registers()[15] - 2U != *entry ||
            (*entry & 15U) != 0U ||
            !horizontal_resample.matches(memory, *entry)) {
            call.resume_original();
            return;
        }
        if ((state & big_endian) != 0U || !try_resample(call))
            call.resume_original_persistently();
    }

} // namespace

void register_core_graphics_resample_hle(UserlandHleRegistry& registry)
{
    registry.register_function(
        "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
        "_resample_byte_h_4cpp_armv7", resample,
        UserlandHleRegistry::SymbolLookup::ImageAndCacheLocals,
        UserlandHleRegistry::EntryPatch::InstructionFetch);
}

} // namespace ilemu
