#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace ilemu {

// The bytes are a project-owned, versioned serialization of Dynarmic's
// optimized IR. They are never native host code and are accepted only after
// bounded A32/x64 emitter-contract validation.
[[nodiscard]] bool validate_dynarmic_ir(std::span<const std::byte> bytes);

} // namespace ilemu
