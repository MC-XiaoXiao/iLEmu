/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
namespace ilemu::execution {
// Select base ARMv6 or the ARMv6T2 integer instruction extensions.
// Shared by multiply and single-transfer decoding; separate from core quirks.
enum class ArmInstructionProfile { Armv6, Armv6T2 };
// ARM-state ALU writes preserve the ISA through ARMv6; ARMv7 interworks.
// Thumb ALU writes always preserve the ISA, independently of this policy.
enum class AluPcWriteProfile { Branch, Interworking };
// VFPv2 and VFPv3-D16 expose D0..D15; full VFPv3 exposes D0..D31.
// Both banks contain the same thirty-two single-precision S registers.
enum class VfpRegisterProfile { D16, D32 };
// VFPv3/v4 exception enables are RAZ/WI. VFPv2 and the U variants
// expose trap controls independently of the size of the register bank.
enum class FloatingExceptionProfile { Untrapped, TrapEnables };
// Strict ARMv7 rejects unpredictable SP operands. The compatibility policy
// executes SP as an ordinary arithmetic or move/shift register. CPU adapters select it
// independently of firmware, guest addresses and backend implementation.
enum class ThumbArithmeticProfile { Architectural, GeneralPurposeStackPointer };
}
