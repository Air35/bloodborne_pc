// SPDX-License-Identifier: GPL-2.0-or-later
/* Host CPU detection and the ISA policy (docs/10, ADR-0010).
 *
 * The guest code (the game's eboot and PRX modules) is compiled for the PS4's AMD Jaguar and runs
 * on the host CPU as it is. A host without one of Jaguar's instruction sets stops with SIGILL
 * somewhere inside the game, or, worse, runs lzcnt/tzcnt as bsr/bsf with other results. This
 * module names the missing sets at start and decides, per set and by CPUID bit (never by
 * vendor), what is native and what is rewritten (isa_rewrite.h).
 *
 * Adapted from gow3 91b6e61 src/cpu_check.{c,h}; OSXSAVE/XCR0 follows the bbport probe check;
 * SSE4a and the policy are new in this project. */
#ifndef SOTC_CPU_CHECK_H
#define SOTC_CPU_CHECK_H

#include <stddef.h>
#include <stdint.h>

/* CPUID words (leaf 1 ECX, leaf 7 subleaf 0 EBX, leaf 0x80000001 ECX) and XCR0 (low 32 bits;
 * 0 when the OS did not enable OSXSAVE). Plain data, so tests can simulate any CPU. */
typedef struct {
    uint32_t leaf1_ecx, leaf7_ebx, ext1_ecx, xcr0;
} CpuFeatureWords;

/* Sets the guest code may use that some hosts lack and that can be rewritten. */
enum {
    CPU_SET_MOVBE = 1u << 0,
    CPU_SET_BMI1 = 1u << 1,
    CPU_SET_LZCNT = 1u << 2,
    CPU_SET_F16C = 1u << 3, /* rewritable in the policy; the stubs do not emulate it yet */
    CPU_SET_SSE4A = 1u << 4,
    CPU_SET_ALL = (1u << 5) - 1,
};

/* Reads CPUID and, when the OS enabled OSXSAVE, XGETBV. */
CpuFeatureWords cpu_feature_words(void);

/* Fatal sets the host lacks (SSE4.2, POPCNT, AVX with OSXSAVE and XCR0 SSE+AVX state), names
 * comma separated in out (cut, never overrun); returns how many. */
int cpu_missing_fatal(CpuFeatureWords words, char* out, size_t size);

/* Rewritable sets the host lacks: CPU_SET_* mask; names in out. */
unsigned cpu_missing_rewritable(CpuFeatureWords words, char* out, size_t size);

/* Names of the sets in mask, comma separated in out; returns how many. */
unsigned cpu_set_names(unsigned mask, char* out, size_t size);

/* SOTC_ISA_MODE (docs/10 §3). */
typedef enum { ISA_POLICY_AUTO, ISA_POLICY_NATIVE, ISA_POLICY_REWRITE } IsaPolicyMode;
typedef struct {
    IsaPolicyMode mode;
    unsigned listed; /* REWRITE: the CPU_SET_* to rewrite even if present (CPU_SET_ALL = all) */
} IsaPolicy;

/* "auto" (also NULL and ""), "native", "rewrite" or "rewrite:<list>" with names movbe, bmi1,
 * lzcnt, f16c, sse4a, case-insensitive. 0 on success; -1 for anything else (the caller stops:
 * a typo must not silently become "auto"). */
int isa_policy_parse(const char* text, IsaPolicy* out);

/* The sets to rewrite for a host that lacks `missing`. A missing set is always rewritten (the
 * alternative is SIGILL), and `rewrite` modes add the listed ones. Returns -1 for `native` when
 * something is missing (the caller prints STOP), else 0. */
int isa_policy_resolve(const IsaPolicy* policy, unsigned missing, unsigned* sets);

#endif
