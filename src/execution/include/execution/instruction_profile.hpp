/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
namespace ilemu::execution {
// Thumb-wide multiply and MLS require the ARMv6T2 instruction extensions.
// ARMv6 cores still support word/long multiply, including A32 UMAAL.
enum class MultiplyProfile { Armv6, Armv6T2 };
// VFPv2 and VFPv3-D16 expose D0..D15; full VFPv3 exposes D0..D31.
// Both banks contain the same thirty-two single-precision S registers.
enum class VfpRegisterProfile { D16, D32 };
// Strict ARMv7 rejects unpredictable SP operands. The compatibility policy
// executes SP as an ordinary arithmetic register. CPU adapters select it
// independently of firmware, guest addresses and backend implementation.
enum class ThumbArithmeticProfile { Architectural, GeneralPurposeStackPointer };
}
