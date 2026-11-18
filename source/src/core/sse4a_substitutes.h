// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <xbyak/xbyak.h>
#include "common/types.h"

// Fast substitutes for the immediate forms of SSE4a EXTRQ and INSERTQ (AMD only; the PS4's CPU
// has them, Intel CPUs do not), for cpu_patches.cpp's trampolines. The general substitutes move
// the value through general registers, which needs the flags saved (pushfq/popfq, ~20+ cycles on
// Intel) and the stack: ~40-60 cycles per instruction. These use SSE2 shifts on the register
// itself (EXTRQ: 3 instructions) and touch neither flags nor general registers. All are legacy
// (non-VEX) encodings like the instructions they replace, so bits 255:128 of the YMM register stay
// as they were. Results equal AMD's, including the upper 64 bits of the XMM register (zeroed);
// tests/test_sse4a_substitutes.cpp compares them with the real instructions on an SSE4a CPU.
namespace Core::Sse4a {

// EXTRQ xmm, length, index: bits [index, index + length) of the low qword, zero-extended
// (length 0 means 64; a field reaching past bit 63 is the qword shifted right by index).
inline void ExtrqImmediate(Xbyak::CodeGenerator& c, const Xbyak::Xmm& dst, u32 length,
                           u32 index) {
    length &= 0x3F;
    index &= 0x3F;
    if (length == 0) {
        length = 64;
    }
    c.movq(dst, dst); // the low qword, the high one zeroed
    if (length + index >= 64) {
        if (index != 0) {
            c.psrlq(dst, static_cast<int>(index));
        }
    } else {
        c.psllq(dst, static_cast<int>(64 - length - index));
        c.psrlq(dst, static_cast<int>(64 - length));
    }
}

// INSERTQ dst, src, length, index: the low `length` bits of src's low qword replace bits
// [index, index + length) of dst's low qword (length 0 means 64; a field reaching past bit 63
// takes the whole source qword shifted left by index); dst's high qword is zeroed.
inline void InsertqImmediate(Xbyak::CodeGenerator& c, const Xbyak::Xmm& dst,
                             const Xbyak::Xmm& src, u32 length, u32 index) {
    using namespace Xbyak::util;
    length &= 0x3F;
    index &= 0x3F;
    if (length == 0) {
        length = 64;
    }
    const bool whole = length + index > 64 || length == 64;
    const u64 mask = whole ? ~u64{0} : (u64{1} << length) - 1;
    const u64 keep = ~(mask << index);
    // A scratch register other than both operands, kept below the guest's red zone.
    int scratch_index = 0;
    while (scratch_index == dst.getIdx() || scratch_index == src.getIdx()) {
        ++scratch_index;
    }
    const Xbyak::Xmm scratch(scratch_index);
    Xbyak::Label code, constant;
    c.jmp(code);
    c.align(16); // legacy PAND needs an aligned 16-byte memory operand
    c.L(constant);
    c.dq(keep);
    c.dq(0); // also zeroes dst's high qword
    c.L(code);
    c.lea(rsp, ptr[rsp - 144]);
    c.movdqu(ptr[rsp], scratch);
    c.movq(scratch, src); // src's low qword, the high one zeroed
    if (whole) {
        if (index != 0) {
            c.psllq(scratch, static_cast<int>(index));
        }
    } else {
        c.psllq(scratch, static_cast<int>(64 - length));
        if (64 - length - index != 0) {
            c.psrlq(scratch, static_cast<int>(64 - length - index));
        }
    }
    c.pand(dst, ptr[rip + constant]);
    c.por(dst, scratch);
    c.movdqu(scratch, ptr[rsp]);
    c.lea(rsp, ptr[rsp + 144]);
}

} // namespace Core::Sse4a
