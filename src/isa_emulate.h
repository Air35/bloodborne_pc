// SPDX-License-Identifier: GPL-2.0-or-later
/* Emulation of guest instructions the host CPU may lack: MOVBE, BMI1 (ANDN, BEXTR, BLSI, BLSMSK,
 * BLSR, TZCNT), LZCNT and SSE4a (EXTRQ, INSERTQ). One table record (IsaSite) describes one
 * instruction site; a stub (isa_rewrite.c) saves the guest's registers and calls isa_emulate.
 *
 * Built with -mgeneral-regs-only and no libc: it runs inside guest code and must leave the
 * XMM/YMM registers untouched (the SSE4a operands arrive spilled in IsaRegs.xmm).
 *
 * Adapted from gow3 91b6e61 src/isa_emulate.{c,h} (audit 2026-10-15); SSE4a is new. */
#ifndef SOTC_ISA_EMULATE_H
#define SOTC_ISA_EMULATE_H

#include <stdint.h>

#ifndef ABI /* same as runtime.h, which needs _GNU_SOURCE for sigjmp_buf */
#define ABI __attribute__((sysv_abi))
#endif

/* Kinds of site. Values are part of the table format: append only. */
enum {
    ISA_MOVBE_LOAD = 1,
    ISA_MOVBE_STORE,
    ISA_LZCNT,
    ISA_TZCNT,
    ISA_ANDN,
    ISA_BEXTR,
    ISA_BLSI,
    ISA_BLSMSK,
    ISA_BLSR,
    ISA_VCVTPH2PS, /* recognized, never emulated: UNSUPPORTED (docs/10 §2) */
    ISA_VCVTPS2PH,
    ISA_EXTRQ,   /* SSE4a: extract a bit field of the low quadword */
    ISA_INSERTQ, /* SSE4a: insert a bit field into the low quadword */
    ISA_KIND_LAST = ISA_INSERTQ,
};
enum { ISA_MODE_JMP = 0, ISA_MODE_JMP_RELOCATE, ISA_MODE_INT3, ISA_MODE_UNSUPPORTED };
enum { ISA_NO_REG = 0xff, ISA_MEM_NONE = 0, ISA_MEM_DST = 1, ISA_MEM_SRC = 2 };
/* IsaSite.mem_flags */
enum {
    ISA_MEM_RIP_RELATIVE = 1, /* disp is an absolute offset into the image */
    ISA_FLAG_IMM = 2,         /* SSE4a: length/index immediates in disp (length | index << 8) */
};

typedef struct { /* one table record (docs/plans/2026-10-15-a1-a3-isa-reescrita.md), as stored */
    uint64_t offset;
    uint8_t length, kind, width, mode, dst, src1, src2, mem_slot, base, index, scale, mem_flags;
    uint8_t reserved[4];
    int64_t disp;
    uint8_t original[15];
    uint8_t relocated_length;
    uint8_t relocated[15];
    uint8_t reserved2;
} IsaSite;
_Static_assert(sizeof(IsaSite) == 64, "table record");

/* The registers the stub saves, in the order it lays them out: rflags, then gpr in x86 numbering
 * (gpr[4] is the guest's rsp), then the two SSE4a operand images: xmm[0] = destination register
 * (low, high quadword), xmm[1] = source register. Only SSE4a sites fill and read the xmm images. */
typedef struct {
    uint64_t rflags;
    uint64_t gpr[16];
    uint64_t xmm[2][2];
} IsaRegs;
_Static_assert(sizeof(IsaRegs) == 8 + 16 * 8 + 32, "stub frame layout");

enum { ISA_CF = 1u << 0, ISA_ZF = 1u << 6, ISA_SF = 1u << 7, ISA_OF = 1u << 11 };

/* Memory operands with mem_flags & ISA_MEM_RIP_RELATIVE address image + disp. */
void isa_emulate_set_image(unsigned char* image);

/* Executes the site's instruction on regs. */
void ABI isa_emulate(const IsaSite* site, IsaRegs* regs);

/* Flags the instruction defines (the tests compare only these). */
uint64_t isa_defined_flags(uint8_t kind);

#endif
