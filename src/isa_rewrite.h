// SPDX-License-Identifier: GPL-2.0-or-later
/* Loader side of the ISA rewrite (docs/10, ADR-0010, ADR-0017).
 *
 * Reads an ISATAB1 table (scripts/isa_rewrite.py), builds one machine-code stub per site in an
 * area next to the image and patches each site with a jmp into its stub (or an int3). The stub
 * saves the guest's registers, calls isa_emulate and jumps back. Runs at load time, before the
 * image is protected; libc is fine here. The stubs are written while the area is RW and must be
 * made RX before the guest runs.
 *
 * Adapted from gow3 91b6e61 src/isa_rewrite.{c,h} (audit 2026-10-15): the validation of
 * RIP-relative operands (audit finding 3), the SSE4a spill/reload and the in-memory parser are
 * new. */
#ifndef SOTC_ISA_REWRITE_H
#define SOTC_ISA_REWRITE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu_check.h"
#include "isa_emulate.h"

/* Table file: 64-byte header, then `count` records of IsaSite (64 bytes), ascending by offset.
 *   +0  char[8]   magic "ISATAB1\0"        +8  u64 image size       +16 u8[32] SHA-256 of image
 *   +48 u32 count                          +52 u32 sites UNSUPPORTED  +56 u32 flags   +60 u32 0 */
enum {
    ISA_TABLE_HEADER_BYTES = 64,
    ISA_TABLE_RECORD_BYTES = 64,
    ISA_TABLE_MAX_SITES = 1u << 20,
    ISA_TABLE_RANGES_MISSING = 1u << 0, /* some module was scanned linearly (no .eh_frame ranges) */
    ISA_STUB_BYTES = 320,
};
#define ISA_TABLE_MAGIC "ISATAB1\0"

typedef struct {
    IsaSite* sites;
    uint64_t* hits; /* hits[i] belongs to sites[i] */
    uint32_t count, unsupported, flags;
    uint64_t image_size;
    uint8_t sha256[32];
    unsigned char *image, *stubs; /* set by isa_rewrite_apply */
} IsaTable;

/* rewritten = jmp + relocated + int3; skipped and unsupported_needed are outside it. */
typedef struct {
    uint32_t rewritten, jmp, relocated, int3, skipped, unsupported_needed;
} IsaReport;

/* Parses an ISATAB1 image in memory. NULL, and one stderr line ("ISA: <reason>"), when invalid. */
IsaTable* isa_table_parse(const unsigned char* data, size_t size);

/* Reads and parses a table file (same errors). */
IsaTable* isa_table_load(const char* path);

void isa_table_free(IsaTable* table);

/* 0 when size and SHA-256 match the image the table was generated from. */
int isa_table_matches(const IsaTable* table, const unsigned char* image, uint64_t size);

size_t isa_stub_area_size(const IsaTable* table); /* count * ISA_STUB_BYTES */

unsigned isa_site_set(const IsaSite* site); /* CPU_SET_* of the kind */

/* Rewrites the sites whose set is in `sets`. Writes nothing and returns unsupported_needed > 0
 * when a needed site is UNSUPPORTED. `stubs` has isa_stub_area_size bytes, RW. A JMP_RELOCATE
 * site whose next instruction no longer matches becomes INT3 (its mode in the table changes);
 * each skipped or downgraded site gets one stderr line. */
IsaReport isa_rewrite_apply(IsaTable* table, unsigned char* image, unsigned char* stubs,
                            unsigned sets);

/* int3 at `address` (the CC byte): 1 when it is a rewritten INT3 site; regs updated and
 * *next_rip = site + its length. 0 otherwise, with regs and *next_rip untouched. */
int isa_rewrite_trap(IsaTable* table, uintptr_t address, IsaRegs* regs, uintptr_t* next_rip);

/* SIGTRAP handler body (POSIX/Linux x86-64): context is the ucontext_t of a SIGTRAP. Returns 1
 * when it emulated an INT3 site and set RIP past it; 0 for any other trap. */
int isa_rewrite_handle_sigtrap(IsaTable* table, void* context);

/* Site index whose stub holds rip, or -1. */
int isa_rewrite_site_of_stub(const IsaTable* table, uintptr_t rip);

/* "ISA: most executed sites:" then up to 10 lines "  0x<offset> <mnemonic> <hits>" (hits > 0
 * only), descending hits, ties by ascending offset. */
void isa_rewrite_report(const IsaTable* table, FILE* out);

/* "movbe", "lzcnt", "tzcnt", "andn", "bextr", "blsi", "blsmsk", "blsr", "vcvtph2ps",
 * "vcvtps2ph", "extrq", "insertq"; "?" otherwise. */
const char* isa_kind_name(uint8_t kind);

#endif
