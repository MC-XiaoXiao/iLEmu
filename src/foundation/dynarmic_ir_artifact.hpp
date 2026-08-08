#pragma once

#include <ilemu/dynarmic_ir_artifact.hpp>

#include <optional>
#include <vector>

#include <dynarmic/ir/basic_block.h>

namespace ilemu {

[[nodiscard]] std::optional<std::vector<std::byte>>
serialize_dynarmic_ir(const Dynarmic::IR::Block& block);

[[nodiscard]] std::optional<Dynarmic::IR::Block> deserialize_dynarmic_ir(
    std::span<const std::byte> bytes);

} // namespace ilemu
