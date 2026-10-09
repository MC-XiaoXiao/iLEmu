/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace ilemu {
class Output;
void run_arm_benchmark(std::uint32_t iterations, std::size_t cache_size,
    std::string_view backend, Output& output);
}
