/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstdint>
namespace ilemu::execution {
// Opaque physical reservation stamp and address-ordered little-endian value.
// No backing pointer or owner escapes. A local monitor also checks the width
// and byte offset before attempting a store through a virtual alias.
struct ExclusiveReservation {
    std::uint64_t value = 0, key = 0;
    std::uint32_t offset = 0, size = 0;
};
}
