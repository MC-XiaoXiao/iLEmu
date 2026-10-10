/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
namespace ilemu::execution {
// VFPv2 and VFPv3-D16 expose D0..D15; full VFPv3 exposes D0..D31.
// Both banks contain the same thirty-two single-precision S registers.
enum class VfpRegisterProfile { D16, D32 };
// Strict ARMv7 rejects unpredictable SP operands. The compatibility policy
// executes SP as an ordinary arithmetic register. CPU adapters select it
// independently of firmware, guest addresses and backend implementation.
enum class ThumbArithmeticProfile { Architectural, GeneralPurposeStackPointer };
}
