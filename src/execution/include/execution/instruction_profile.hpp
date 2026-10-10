/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
namespace ilemu::execution {
// Strict ARMv7 rejects unpredictable SP operands. The compatibility policy
// executes SP as an ordinary arithmetic register. CPU adapters select it
// independently of firmware, guest addresses and backend implementation.
enum class ThumbArithmeticProfile { Architectural, GeneralPurposeStackPointer };
}
